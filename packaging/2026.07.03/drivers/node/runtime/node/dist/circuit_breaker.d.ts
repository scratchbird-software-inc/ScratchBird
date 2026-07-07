export type CircuitState = "CLOSED" | "OPEN" | "HALF_OPEN";
export interface CircuitBreakerConfig {
    failureThreshold?: number;
    recoveryTimeoutMs?: number;
    successThreshold?: number;
    halfOpenMaxRequests?: number;
}
export declare class CircuitBreakerOpenError extends Error {
    constructor(message: string);
}
export declare class CircuitBreaker {
    private readonly name;
    private readonly config;
    private state;
    private failureCount;
    private successCount;
    private halfOpenRequests;
    private lastFailureAt;
    constructor(config?: CircuitBreakerConfig, name?: string);
    getState(): CircuitState;
    allowRequest(): boolean;
    recordSuccess(): void;
    recordFailure(): void;
    reset(): void;
    stats(): Record<string, unknown>;
    private allowHalfOpenRequest;
    private transitionToHalfOpen;
    private transitionToOpen;
    private transitionToClosed;
}
