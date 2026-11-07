from enum import Enum


class PostApiAuthProvidersProviderAuthorizeProvider(str, Enum):
    GITHUB = "github"
    GOOGLE = "google"
    WECHAT = "wechat"

    def __str__(self) -> str:
        return str(self.value)
