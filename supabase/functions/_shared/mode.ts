// Pipeline mode: PIPELINE_MODE = ai | loopback | auto (default). "auto" runs the AI pipeline
// only when both provider keys are configured as Edge Function secrets, so deploying without
// keys keeps the Milestone 1 loopback behaviour.
export type Mode = "ai" | "loopback";

export function resolveMode(env: { get(k: string): string | undefined }): { mode: Mode; deepgramKey: string; geminiKey: string } {
  const deepgramKey = env.get("DEEPGRAM_API_KEY") ?? "";
  const geminiKey = env.get("GEMINI_API_KEY") ?? "";
  const wanted = (env.get("PIPELINE_MODE") ?? "auto").toLowerCase();
  const haveKeys = deepgramKey.length > 0 && geminiKey.length > 0;
  if (wanted === "ai" && !haveKeys) throw new Error("PIPELINE_MODE=ai but DEEPGRAM_API_KEY / GEMINI_API_KEY are not set");
  const mode: Mode = wanted === "loopback" ? "loopback" : wanted === "ai" || haveKeys ? "ai" : "loopback";
  return { mode, deepgramKey, geminiKey };
}
