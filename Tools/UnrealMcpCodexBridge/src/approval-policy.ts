export type ApprovalMode = "reject" | "auto-approve";

const rejectMessage =
  "UEvolve Codex Bridge V1 rejects file, command, permission, elicitation, and user-input requests.";

export function approvalModeFromEnv(): ApprovalMode {
  return process.env.UEVOLVE_CODEX_APPROVAL_POLICY === "auto-approve"
    ? "auto-approve"
    : "reject";
}

export function approvalResponse(method: string, params: any, mode: ApprovalMode): any {
  const approve = mode === "auto-approve";
  switch (method) {
    case "applyPatchApproval":
    case "execCommandApproval":
      return { decision: approve ? "approved" : "denied" };
    case "item/commandExecution/requestApproval":
      return { decision: approve ? "accept" : "decline" };
    case "item/fileChange/requestApproval":
      return { decision: approve ? "accept" : "decline" };
    case "item/permissions/requestApproval":
      return {
        permissions: approve ? params?.permissions ?? {} : {},
        scope: "turn",
        strictAutoReview: !approve,
      };
    case "mcpServer/elicitation/request":
      return { action: approve ? "accept" : "decline", content: null, _meta: { message: rejectMessage } };
    case "item/tool/requestUserInput":
      return { answers: {} };
    case "item/tool/call":
      return {
        contentItems: [{ type: "inputText", text: approve ? "No dynamic tool handler is installed." : rejectMessage }],
        success: false,
      };
    default:
      return {};
  }
}
