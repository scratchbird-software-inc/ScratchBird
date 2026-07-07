export interface LeakDetectionConfig {
    thresholdMs?: number;
    captureStackTrace?: boolean;
    checkIntervalMs?: number;
}
export declare class CheckoutInfo {
    readonly metadata: Record<string, unknown>;
    readonly checkoutTime: number;
    readonly stackTrace?: string;
    constructor(metadata: Record<string, unknown>, captureStackTrace: boolean);
    heldDurationMs(): number;
}
export declare class LeakDetectionGuard {
    private readonly detector;
    private readonly connectionId;
    private released;
    constructor(detector: LeakDetector, connectionId: string);
    release(): void;
}
export declare class LeakDetector {
    private readonly config;
    private readonly checkouts;
    private timer?;
    constructor(config?: LeakDetectionConfig);
    start(): void;
    stop(): void;
    checkout(connectionId: string, metadata?: Record<string, unknown>): LeakDetectionGuard;
    checkin(connectionId: string): void;
    activeCount(): number;
    stats(): Record<string, unknown>;
    private checkLeaks;
}
