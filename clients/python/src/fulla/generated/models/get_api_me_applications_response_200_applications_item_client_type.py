from enum import Enum


class GetApiMeApplicationsResponse200ApplicationsItemClientType(str, Enum):
    CONFIDENTIAL = "CONFIDENTIAL"
    PUBLIC = "PUBLIC"

    def __str__(self) -> str:
        return str(self.value)
