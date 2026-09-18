from enum import Enum


class GetApiMeOrganizationsSlugMembersResponse200MembersItemRole(str, Enum):
    ADMIN = "admin"
    MEMBER = "member"
    OWNER = "owner"

    def __str__(self) -> str:
        return str(self.value)
