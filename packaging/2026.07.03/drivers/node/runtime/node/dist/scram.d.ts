export type ScramAlgorithm = "sha256" | "sha512";
export declare class ScramExchange {
    private username;
    private algorithm;
    private clientNonce;
    private clientFirstBare;
    private serverSignature?;
    constructor(username: string, algorithm?: ScramAlgorithm, nonce?: string);
    clientFirstMessage(): string;
    handleServerFirst(password: string, serverFirst: string): string;
    verifyServerFinal(serverFinal: string): void;
}
