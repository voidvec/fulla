from enum import Enum


class PostApiMeApplicationsBodyClientType(str, Enum):
    CONFIDENTIAL = "CONFIDENTIAL"
    PUBLIC = "PUBLIC"

    def __str__(self) -> str:
        return str(self.value)
