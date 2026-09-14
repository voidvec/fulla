package auth

import (
	"context"
	"encoding/base64"
	"fmt"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"sync"
	"testing"
	"time"
)

// fakeTokenServer serves /oauth2/token with counting and canned behaviour.
type fakeTokenServer struct {
	mu          sync.Mutex
	tokenHits   int
	forms       []url.Values
	authHeaders []string
	status      int // non-0 => error body with this status
	expiresIn   int // 0 => default 3600
	accessToken string
}

func newFakeTokenServer() *fakeTokenServer {
	return &fakeTokenServer{expiresIn: 3600}
}

func (f *fakeTokenServer) hitCount() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.tokenHits
}

func (f *fakeTokenServer) handler(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/oauth2/token" {
		http.NotFound(w, r)
		return
	}
	// Capture the request shape INSIDE the handler: the body is closed once
	// the response is written, so PostForm cannot be parsed afterwards.
	if err := r.ParseForm(); err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	f.mu.Lock()
	f.tokenHits++
	f.forms = append(f.forms, r.PostForm)
	f.authHeaders = append(f.authHeaders, r.Header.Get("Authorization"))
	status := f.status
	expiresIn := f.expiresIn
	access := f.accessToken
	hits := f.tokenHits
	f.mu.Unlock()
	if status != 0 {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(status)
		fmt.Fprintf(w, `{"error":"invalid_client","error_description":"bad credentials"}`)
		return
	}
	if access == "" {
		access = fmt.Sprintf("at-%d", hits)
	}
	w.Header().Set("Content-Type", "application/json")
	fmt.Fprintf(w, `{"access_token":%q,"token_type":"Bearer","expires_in":%d}`, access, expiresIn)
}

func (f *fakeTokenServer) lastForm(t *testing.T) url.Values {
	t.Helper()
	f.mu.Lock()
	defer f.mu.Unlock()
	if len(f.forms) == 0 {
		t.Fatal("no token requests recorded")
	}
	return f.forms[len(f.forms)-1]
}

func (f *fakeTokenServer) lastAuthHeader(t *testing.T) string {
	t.Helper()
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.authHeaders[len(f.authHeaders)-1]
}

func decodeBasic(t *testing.T, header string) string {
	t.Helper()
	const prefix = "Basic "
	if !strings.HasPrefix(header, prefix) {
		t.Fatalf("expected Basic auth header, got %q", header)
	}
	raw, err := base64.StdEncoding.DecodeString(strings.TrimPrefix(header, prefix))
	if err != nil {
		t.Fatalf("decode basic: %v", err)
	}
	return string(raw)
}

// G1 -- token fetch: Basic auth shape + form fields.
func TestFetchClientCredentialsTokenBasicAndForm(t *testing.T) {
	fake := newFakeTokenServer()
	srv := httptest.NewServer(http.HandlerFunc(fake.handler))
	defer srv.Close()

	token, err := FetchClientCredentialsToken(context.Background(), srv.URL, "backend-svc", "test-secret", []string{"tokens:read", "tokens:write"})
	if err != nil {
		t.Fatalf("fetch: %v", err)
	}
	if token.AccessToken == "" {
		t.Fatal("no access token")
	}
	if got := decodeBasic(t, fake.lastAuthHeader(t)); got != "backend-svc:test-secret" {
		t.Fatalf("basic credentials = %q", got)
	}
	form := fake.lastForm(t)
	if form.Get("grant_type") != "client_credentials" {
		t.Fatalf("grant_type = %q", form.Get("grant_type"))
	}
	if form.Get("scope") != "tokens:read tokens:write" {
		t.Fatalf("scope = %q", form.Get("scope"))
	}
}

// G2 -- TokenSource caches: one fetch across concurrent use.
func TestTokenSourceCachesAcrossConcurrency(t *testing.T) {
	fake := newFakeTokenServer()
	srv := httptest.NewServer(http.HandlerFunc(fake.handler))
	defer srv.Close()

	ctx := context.Background()
	client, err := NewM2MClient(ctx, srv.URL, "c", "s", nil)
	if err != nil {
		t.Fatalf("client: %v", err)
	}
	// The generated client hitting any endpoint goes through the shared
	// x/oauth2 token source.
	var wg sync.WaitGroup
	for i := 0; i < 10; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			resp, err := client.GetWellKnownOpenidConfiguration(ctx)
			if err == nil {
				resp.Body.Close()
			}
		}()
	}
	wg.Wait()
	if fake.hitCount() != 1 {
		t.Fatalf("token endpoint hits = %d, want 1 (cached)", fake.hitCount())
	}
}

// G3 -- expired tokens transparently refresh on the next request.
func TestExpiredTokenAutoRefreshes(t *testing.T) {
	fake := newFakeTokenServer()
	fake.expiresIn = 1 // expires immediately
	srv := httptest.NewServer(http.HandlerFunc(fake.handler))
	defer srv.Close()

	ctx := context.Background()
	client, err := NewM2MClient(ctx, srv.URL, "c", "s", nil)
	if err != nil {
		t.Fatalf("client: %v", err)
	}
	if resp, err := client.GetWellKnownOpenidConfiguration(ctx); err == nil {
		resp.Body.Close()
	}
	// Wait past the (1 s) token lifetime; ReuseTokenSource must refetch.
	time.Sleep(1300 * time.Millisecond)
	if resp, err := client.GetWellKnownOpenidConfiguration(ctx); err == nil {
		resp.Body.Close()
	}
	if fake.hitCount() < 2 {
		t.Fatalf("token endpoint hits = %d, want >= 2 (auto refresh)", fake.hitCount())
	}
}

// G4 -- token-endpoint failures surface as *AuthError with the RFC 6749 body.
func TestTokenErrorPropagation(t *testing.T) {
	fake := newFakeTokenServer()
	fake.status = 401
	srv := httptest.NewServer(http.HandlerFunc(fake.handler))
	defer srv.Close()

	_, err := FetchClientCredentialsToken(context.Background(), srv.URL, "c", "bad", nil)
	if err == nil {
		t.Fatal("expected error")
	}
	authErr, ok := err.(*AuthError)
	if !ok {
		t.Fatalf("error type = %T, want *AuthError", err)
	}
	if authErr.ErrorCode != "invalid_client" {
		t.Fatalf("error code = %q", authErr.ErrorCode)
	}
	if authErr.ErrorDescription != "bad credentials" {
		t.Fatalf("error description = %q", authErr.ErrorDescription)
	}
	if authErr.StatusCode != 401 {
		t.Fatalf("status = %d", authErr.StatusCode)
	}
}

// G5 -- RFC 7636 appendix B S256 vector.
func TestPkceRfc7636Vector(t *testing.T) {
	verifier := "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"
	want := "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"
	if got := CreateChallenge(verifier); got != want {
		t.Fatalf("challenge = %q, want %q", got, want)
	}
}

// G5b -- generated pair shape and non-determinism.
func TestPkcePairShape(t *testing.T) {
	p1, err := NewPkcePair()
	if err != nil {
		t.Fatalf("pair: %v", err)
	}
	p2, _ := NewPkcePair()
	if l := len(p1.Verifier); l < 43 || l > 128 {
		t.Fatalf("verifier length = %d", l)
	}
	if p1.Challenge != CreateChallenge(p1.Verifier) {
		t.Fatal("challenge mismatch")
	}
	if p1.Verifier == p2.Verifier {
		t.Fatal("verifiers must not repeat")
	}
}

// G6 -- BuildAuthorizeURL carries every parameter.
func TestBuildAuthorizeURL(t *testing.T) {
	flow := NewAuthCodeFlow("http://server.test/", "fulla-portal", "", "http://client.test/cb", []string{"openid", "profile"})
	pkce, _ := NewPkcePair()
	raw, err := flow.BuildAuthorizeURL("st4te", &pkce)
	if err != nil {
		t.Fatalf("build: %v", err)
	}
	u, err := url.Parse(raw)
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if u.Scheme != "http" || u.Host != "server.test" || u.Path != "/oauth2/authorize" {
		t.Fatalf("url = %q", raw)
	}
	q := u.Query()
	if q.Get("response_type") != "code" ||
		q.Get("client_id") != "fulla-portal" ||
		q.Get("redirect_uri") != "http://client.test/cb" ||
		q.Get("scope") != "openid profile" ||
		q.Get("state") != "st4te" ||
		q.Get("code_challenge") != pkce.Challenge ||
		q.Get("code_challenge_method") != "S256" {
		t.Fatalf("params = %q", q)
	}
}

// G8 -- confidential exchange/refresh use HTTP Basic; public clients send
// client_id in the form instead (F-017 mirror).
func TestExchangeAndRefreshUseBasic(t *testing.T) {
	fake := newFakeTokenServer()
	srv := httptest.NewServer(http.HandlerFunc(fake.handler))
	defer srv.Close()

	confidential := NewAuthCodeFlow(srv.URL, "backend-svc", "test-secret", "http://client.test/cb", nil)
	tokens, err := confidential.ExchangeCode(context.Background(), "auth-code-1", "ver-ifr")
	if err != nil {
		t.Fatalf("exchange: %v", err)
	}
	if tokens.AccessToken == "" {
		t.Fatal("no access token")
	}
	if got := decodeBasic(t, fake.lastAuthHeader(t)); got != "backend-svc:test-secret" {
		t.Fatalf("basic = %q", got)
	}
	form := fake.lastForm(t)
	if form.Get("grant_type") != "authorization_code" || form.Get("code") != "auth-code-1" || form.Get("code_verifier") != "ver-ifr" {
		t.Fatalf("form = %q", form)
	}
	if form.Get("client_secret") != "" || form.Get("client_id") != "" {
		t.Fatalf("credentials must not leak into the form body: %q", form)
	}

	if _, err := confidential.Refresh(context.Background(), "rt-old"); err != nil {
		t.Fatalf("refresh: %v", err)
	}
	form = fake.lastForm(t)
	if form.Get("grant_type") != "refresh_token" || form.Get("refresh_token") != "rt-old" {
		t.Fatalf("refresh form = %q", form)
	}

	public := NewAuthCodeFlow(srv.URL, "fulla-portal", "", "http://client.test/cb", nil)
	if _, err := public.ExchangeCode(context.Background(), "auth-code-2", ""); err != nil {
		t.Fatalf("public exchange: %v", err)
	}
	if h := fake.lastAuthHeader(t); h != "" {
		t.Fatalf("public client must not send Basic, got %q", h)
	}
	if form := fake.lastForm(t); form.Get("client_id") != "fulla-portal" {
		t.Fatalf("public client_id form = %q", form)
	}
}

// G8b -- exchange failures raise *AuthError.
func TestExchangeErrorSurfacesAuthError(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(400)
		fmt.Fprint(w, `{"error":"invalid_grant","error_description":"code expired"}`)
	}))
	defer srv.Close()

	flow := NewAuthCodeFlow(srv.URL, "backend-svc", "test-secret", "", nil)
	_, err := flow.ExchangeCode(context.Background(), "stale", "")
	authErr, ok := err.(*AuthError)
	if !ok {
		t.Fatalf("error type = %T", err)
	}
	if authErr.ErrorCode != "invalid_grant" {
		t.Fatalf("code = %q", authErr.ErrorCode)
	}
}

func TestParseAuthorizationResponse(t *testing.T) {
	code, state, errCode, err := ParseAuthorizationResponse("http://client.test/cb?code=abc&state=st4te")
	if err != nil || code != "abc" || state != "st4te" || errCode != "" {
		t.Fatalf("got %q %q %q %v", code, state, errCode, err)
	}
	code, state, errCode, err = ParseAuthorizationResponse("http://client.test/cb?error=access_denied&state=st4te")
	if err != nil || code != "" || errCode != "access_denied" || state != "st4te" {
		t.Fatalf("got %q %q %q %v", code, state, errCode, err)
	}
}
