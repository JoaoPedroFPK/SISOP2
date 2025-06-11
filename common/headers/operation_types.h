#ifndef OPERATION_TYPES_H
#define OPERATION_TYPES_H

enum class OperationType {
    FILE_UPLOAD,
    FILE_DELETE,
    FILE_DOWNLOAD,  // For tracking access patterns
    USER_LOGIN,
    SYNC_STATE
};

#endif // OPERATION_TYPES_H 