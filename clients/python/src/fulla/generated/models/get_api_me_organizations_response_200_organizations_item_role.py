from enum import Enum


class GetApiMeOrganizationsResponse200OrganizationsItemRole(str, Enum):
    ADMIN = "admin"
    MEMBER = "member"
    OWNER = "owner"

    def __str__(self) -> str:
        return str(self.value)
