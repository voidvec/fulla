// P0-9 (#263): go-oidc + x/oauth2 RP smoke against fulla.
//
// Stages: provider discovery -> authorization URL (own state/nonce/PKCE
// S256) -> json-mode first-factor login -> oauth2 Exchange (PKCE) ->
// IDToken.Verify (JWKS signature, iss/aud/exp + custom nonce claim check) ->
// provider.UserInfo (sub consistency) -> oauth2 TokenSource refresh ->
// end-session. Any error fails the run.
package main

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"strings"

	"github.com/coreos/go-oidc/v3/oidc"
	"golang.org/x/oauth2"
)

var (
	base      = env("FULLA_BASE_URL", "http://localhost:5555")
	clientID  = env("FULLA_RP_CLIENT_ID", "fulla-portal")
	redirect  = env("FULLA_RP_REDIRECT_URI", "http://localhost:5173/callback")
	scope     = env("FULLA_RP_SCOPE", "openid profile email")
	user      = env("FULLA_RP_USER", "admin")
	password  = env("FULLA_RP_PASSWORD", "admin")
)

func env(k, d string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return d
}

func fail(stage string, err error) {
	fmt.Fprintf(os.Stderr, "[go-oidc] FAIL %s: %v\n", stage, err)
	os.Exit(1)
}

func ok(stage string) { fmt.Printf("[go-oidc] ok   %s\n", stage) }

type idTokenClaims struct {
	Nonce string `json:"nonce"`
}

func main() {
	ctx := context.Background()

	provider, err := oidc.NewProvider(ctx, base)
	if err != nil {
		fail("discovery", err)
	}
	ok("discovery")

	verifierBytes := make([]byte, 32)
	if _, err := rand.Read(verifierBytes); err != nil {
		fail("pkce", err)
	}
	codeVerifier := base64.RawURLEncoding.EncodeToString(verifierBytes)
	challengeSum := sha256.Sum256([]byte(codeVerifier))
	codeChallenge := base64.RawURLEncoding.EncodeToString(challengeSum[:])

	config := oauth2.Config{
		ClientID:    clientID,
		Endpoint:   provider.Endpoint(),
		RedirectURL: redirect,
		Scopes:     strings.Split(scope, " "),
	}
	state := base64.RawURLEncoding.EncodeToString(verifierBytes[:8])
	nonce := base64.RawURLEncoding.EncodeToString(verifierBytes[8:20])
	authzURL := config.AuthCodeURL(state,
		oauth2.SetAuthURLParam("code_challenge", codeChallenge),
		oauth2.SetAuthURLParam("code_challenge_method", "S256"),
		oauth2.SetAuthURLParam("nonce", nonce),
	)
	ok("authorization url")

	// First factor via fulla's json-mode login (flattened authorize params).
	q, err := url.Parse(authzURL)
	if err != nil {
		fail("parse authz url", err)
	}
	sp := q.Query()
	form := url.Values{
		"username":              {user},
		"password":              {password},
		"client_id":             {sp.Get("client_id")},
		"redirect_uri":          {sp.Get("redirect_uri")},
		"scope":                 {sp.Get("scope")},
		"state":                 {sp.Get("state")},
		"nonce":                 {sp.Get("nonce")},
		"code_challenge":        {sp.Get("code_challenge")},
		"code_challenge_method": {sp.Get("code_challenge_method")},
	}
	loginResp, err := http.PostForm(base+"/oauth2/login?json=true", form)
	if err != nil {
		fail("login", err)
	}
	defer loginResp.Body.Close()
	var login struct {
		Code string `json:"code"`
	}
	body, _ := io.ReadAll(loginResp.Body)
	if err := json.Unmarshal(body, &login); err != nil || login.Code == "" {
		fail("login", fmt.Errorf("status %d: %s", loginResp.StatusCode, string(body)[:min(len(body), 300)]))
	}
	ok("login (code issued)")

	tok, err := config.Exchange(ctx, login.Code,
		oauth2.SetAuthURLParam("code_verifier", codeVerifier),
	)
	if err != nil {
		fail("exchange", err)
	}
	ok("oauth2 exchange (PKCE)")

	idTokenRaw, hasID := tok.Extra("id_token").(string)
	if !hasID {
		fail("id_token", fmt.Errorf("no id_token in token response"))
	}
	verifier := provider.Verifier(&oidc.Config{ClientID: clientID})
	idt, err := verifier.Verify(ctx, idTokenRaw)
	if err != nil {
		fail("IDToken.Verify", err)
	}
	var claims idTokenClaims
	if err := idt.Claims(&claims); err != nil {
		fail("id_token claims", err)
	}
	if claims.Nonce != nonce {
		fail("id_token nonce", fmt.Errorf("got %q want %q", claims.Nonce, nonce))
	}
	ok("IDToken.Verify (JWKS sig/iss/aud/exp + nonce)")

	ui, err := provider.UserInfo(ctx, oauth2.StaticTokenSource(tok))
	if err != nil {
		fail("UserInfo", err)
	}
	if ui.Subject != idt.Subject {
		fail("UserInfo sub", fmt.Errorf("id_token %q vs userinfo %q", idt.Subject, ui.Subject))
	}
	ok(fmt.Sprintf("UserInfo (sub consistent: %s…)", idt.Subject[:8]))

	refreshed, err := config.TokenSource(ctx, tok).Token()
	if err != nil {
		fail("refresh", err)
	}
	if refreshed.AccessToken == "" {
		fail("refresh", fmt.Errorf("empty access token"))
	}
	ok("oauth2 refresh (rotation)")

	endURL := base + "/oauth2/end_session?id_token_hint=" + url.QueryEscape(idTokenRaw) +
		"&post_logout_redirect_uri=" + url.QueryEscape(redirect)
	noRedirect := &http.Client{CheckRedirect: func(req *http.Request, via []*http.Request) error {
		return http.ErrUseLastResponse
	}}
	endResp, err := noRedirect.Get(endURL)
	if err != nil {
		fail("end_session", err)
	}
	endResp.Body.Close()
	if endResp.StatusCode != http.StatusFound {
		fail("end_session", fmt.Errorf("status %d (want 302)", endResp.StatusCode))
	}
	if loc := endResp.Header.Get("Location"); loc == "" || !strings.HasPrefix(loc, redirect) {
		fail("end_session", fmt.Errorf("unexpected Location %q", loc))
	}
	ok("end_session (302 -> post_logout redirect)")

	fmt.Println("[go-oidc] ALL STAGES GREEN")
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}
