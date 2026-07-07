export interface KeepaliveConfig {
    intervalMs?: number;
    maxIdleBeforeCheckMs?: number;
    validationTimeoutMs?: number;
}
export declare class KeepaliveTracker {
    private readonly config;
    private lastActivity;
    constructor(config: Required<KeepaliveConfig>);
    markActive(): void;
    needsValidation(): boolean;
    idleDurationMs(): number;
}
type Pinger = () => boolean | Promise<boolean>;
export declare class KeepaliveManager {
    private readonly config;
    private readonly trackers;
    private readonly pingers;
    private timer?;
    constructor(config?: KeepaliveConfig);
    start(): void;
    stop(): void;
    register(connId: string, pinger: Pinger): KeepaliveTracker;
    unregister(connId: string): void;
    private checkConnections;
    private pingWithTimeout;
}
export {};
