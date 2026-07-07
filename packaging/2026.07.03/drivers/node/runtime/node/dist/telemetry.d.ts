export interface TelemetryConfig {
    enableTracing?: boolean;
    enableMetrics?: boolean;
    enableSlowQueryLog?: boolean;
    slowQueryThresholdMs?: number;
    sanitizeQueries?: boolean;
    sampleRate?: number;
}
export declare class SpanContext {
    readonly traceId: string;
    readonly spanId: string;
    readonly parentSpanId?: string;
    readonly spanName: string;
    readonly startTime: number;
    readonly attributes: Record<string, string>;
    constructor(name: string, parent?: SpanContext);
    withAttribute(key: string, value: string): SpanContext;
    elapsedMs(): number;
}
export declare class TelemetryCollector {
    private readonly config;
    private spans;
    private totalQueries;
    private successfulQueries;
    private failedQueries;
    private totalQueryTimeMs;
    private histogram;
    private operationMetrics;
    private slowQueries;
    constructor(config?: TelemetryConfig);
    startSpan(name: string): SpanContext | null;
    endSpan(span: SpanContext | null, success?: boolean): void;
    metrics(): Record<string, unknown>;
    slowQueryLog(): Array<Record<string, unknown>>;
    static sanitizeQuery(sql?: string | null): string;
    exportPrometheusMetrics(): string;
    private recordQueryMetrics;
    private recordSlowQuery;
}
