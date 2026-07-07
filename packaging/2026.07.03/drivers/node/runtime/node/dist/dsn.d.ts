import { ClientConfig } from "./types";
declare function normalizeNativeProtocol(value?: string): string;
declare function normalizeFrontDoorMode(value?: string): "direct" | "manager_proxy";
export { normalizeNativeProtocol, normalizeFrontDoorMode };
export declare function parseDsn(dsn?: string): Partial<ClientConfig>;
