export type ApprovalMode = "reject" | "auto-approve";

// These callbacks cover Codex App Server requests routed to this host for
// built-in OS-level capabilities and user interaction. Unreal MCP tool calls
// are issued through the app-server's mcpServer/tool/call path and do not use
// these approval gates, except Codex CLI 0.130.0 represents MCP tool-call
// confirmation as a form-mode MCP elicitation with codex_approval_kind set to
// mcp_tool_call. That narrow case is accepted only for UEVOLVE_MCP_NAME.
const rejectMessage =
  "UEvolve Codex Bridge rejects built-in file, command, permission, elicitation, and user-input requests. Use Unreal MCP tools instead.";

export function approvalModeFromEnv(): ApprovalMode {
  return process.env.UEVOLVE_CODEX_APPROVAL_POLICY === "auto-approve"
    ? "auto-approve"
    : "reject";
}

export function approvalResponse(method: string, params: any, mode: ApprovalMode): any {
  switch (method) {
    case "applyPatchApproval":
    case "execCommandApproval":
      return { decision: "denied" };
    case "item/commandExecution/requestApproval":
      return { decision: "decline" };
    case "item/fileChange/requestApproval":
      return { decision: "decline" };
    case "item/permissions/requestApproval":
      return {
        permissions: {},
        scope: "turn",
        strictAutoReview: true,
      };
    case "mcpServer/elicitation/request":
      if (isAllowedMcpToolCallApproval(params)) return { action: "accept", content: {}, _meta: null };
      return { action: "decline", content: null, _meta: { message: rejectMessage } };
    case "item/tool/requestUserInput":
      return { answers: {} };
    case "item/tool/call":
      return {
        contentItems: [{ type: "inputText", text: rejectMessage }],
        success: false,
      };
    default:
      return {};
  }
}

function isAllowedMcpToolCallApproval(params: any): boolean {
  const allowedServer = process.env.UEVOLVE_MCP_NAME ?? "unrealmcp";
  return params?.serverName === allowedServer && params?._meta?.codex_approval_kind === "mcp_tool_call";
}
