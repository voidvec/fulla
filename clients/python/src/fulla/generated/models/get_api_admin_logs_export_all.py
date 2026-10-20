from enum import Enum


class GetApiAdminLogsExportAll(str, Enum):
    TRUE = "true"
    VALUE_1 = "1"

    def __str__(self) -> str:
        return str(self.value)
