from enum import Enum


class PostOauth2RegisterBodyClientType(str, Enum):
    CONFIDENTIAL = "CONFIDENTIAL"
    PUBLIC = "PUBLIC"

    def __str__(self) -> str:
        return str(self.value)
