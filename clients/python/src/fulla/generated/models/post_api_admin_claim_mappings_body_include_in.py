from enum import Enum


class PostApiAdminClaimMappingsBodyIncludeIn(str, Enum):
    BOTH = "both"
    ID_TOKEN = "id_token"
    USERINFO = "userinfo"

    def __str__(self) -> str:
        return str(self.value)
