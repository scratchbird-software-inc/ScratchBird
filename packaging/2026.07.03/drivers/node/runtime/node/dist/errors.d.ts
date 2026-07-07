export declare class ScratchbirdError extends Error {
    code?: string;
    detail?: string;
    hint?: string;
    constructor(message: string, code?: string, detail?: string, hint?: string);
}
export declare class ScratchbirdWarning extends ScratchbirdError {
}
export declare class ScratchbirdNoDataError extends ScratchbirdError {
}
export declare class ScratchbirdConnectionError extends ScratchbirdError {
}
export declare class ScratchbirdNotSupportedError extends ScratchbirdError {
}
export declare class ScratchbirdDataError extends ScratchbirdError {
}
export declare class ScratchbirdIntegrityError extends ScratchbirdError {
}
export declare class ScratchbirdAuthError extends ScratchbirdError {
}
export declare class ScratchbirdTransactionError extends ScratchbirdError {
}
export declare class ScratchbirdSyntaxError extends ScratchbirdError {
}
export declare class ScratchbirdResourceError extends ScratchbirdError {
}
export declare class ScratchbirdLimitError extends ScratchbirdError {
}
export declare class ScratchbirdOperatorInterventionError extends ScratchbirdError {
}
export declare class ScratchbirdSystemError extends ScratchbirdError {
}
export declare class ScratchbirdInternalError extends ScratchbirdError {
}
export type RetryScope = "none" | "reconnect" | "statement" | "transaction";
export declare function mapSqlState(code?: string): new (...args: any[]) => ScratchbirdError;
export declare function retryScopeForSqlState(code?: string): RetryScope;
export declare function isRetryableSqlState(code?: string): boolean;
