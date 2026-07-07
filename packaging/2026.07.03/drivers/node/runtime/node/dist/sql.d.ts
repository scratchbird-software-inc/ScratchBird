export interface NormalizedQuery {
    sql: string;
    params: any[];
}
export interface PreparedQueryPlan {
    sql: string;
    paramCount: number;
    namedOrder: string[] | null;
}
export declare function normalizeQuery(sql: string, params?: any[] | Record<string, any>): NormalizedQuery;
export declare function normalizeCallableQuery(sql: string, params?: any[] | Record<string, any>): NormalizedQuery;
export declare function normalizePreparedQuery(sql: string): PreparedQueryPlan;
export declare function normalizeCallableSql(sql: string): string;
/**
 * Split SQL into top-level statements on the active terminator.
 *
 * Quote-aware (single/double quotes) and `--` line-comment aware. Honors the
 * `SET TERM <terminator>` client directive: the
 * directive changes the active terminator and is consumed — it is not emitted as
 * a statement and is not counted in statement indexing. This lets procedural
 * bodies (functions, procedures, triggers) contain inner `;` between
 * `SET TERM ^` and the restoring `SET TERM ;^`.
 *
 * With no `SET TERM` directive present, the behavior is identical to a plain
 * quote-aware top-level `;` split, so existing scripts and statement indices are
 * unchanged. (The chosen terminator must not appear in the bodies it wraps.)
 */
export declare function splitTopLevelStatements(sql: string): string[];
