from enum import IntEnum


class SocialLoginAuthorizationExpiresIn(IntEnum):
    VALUE_600 = 600

    def __str__(self) -> str:
        return str(self.value)
