from enum import Enum


class AuthProviderType(str, Enum):
    OIDC = "oidc"

    def __str__(self) -> str:
        return str(self.value)
