#!/usr/bin/env node
// security-policy-audit.js — 安全策略文件门禁（S-SEC-B10-007）
//
// 检测仓库是否具备安全治理基线：
//   1. 仓库根存在 SECURITY.md（安全策略：漏洞报告 / 安全边界 / 功能清单与默认状态 / 文档映射）
//   2. docs/security/enablement-plan.md 存在且含必需章节锚点（最小启用方案：默认开/关清单表 + 启用建议 + 验收说明）
//
// RED（退出码 1）：任一必需文件缺失，或 SECURITY.md / enablement-plan.md 缺少必需章节锚点。
// GREEN（退出码 0）：全部齐备。
// 用法：node scripts/security-policy-audit.js [--ci]
//   --ci  最小输出、以退出码 1 表示 RED，供门禁/CI 直接消费。
const fs = require("fs");
const path = require("path");

const repoRoot = path.resolve(__dirname, "..");
const SECURITY_MD = path.join(repoRoot, "SECURITY.md");
const ENABLEMENT_PLAN_MD = path.join(repoRoot, "docs", "security", "enablement-plan.md");

// SECURITY.md 必需章节锚点（与文件实际标题保持一致；大小写不敏感、忽略全部空白）。
// alsoNeed：锚点之外还需同时出现的关键词（用于"功能清单需含默认状态"这类复合校验）。
const REQUIRED_ANCHORS = [
  { key: "report", name: "漏洞报告章节", needle: "报告安全漏洞" },
  { key: "boundary", name: "安全边界章节", needle: "安全边界" },
  { key: "inventory", name: "安全功能清单（含默认状态）", needle: "安全功能清单", alsoNeed: "默认" },
  { key: "mapping", name: "docs/security 文档映射", needle: "docs/security/" },
];

// docs/security/enablement-plan.md 必需章节锚点（与 SECURITY.md 同等强度）：
// 最小启用方案须含"默认开/关清单"（alsoNeed 当前状态 = 默认状态表列）、"启用建议"与"验收说明"。
const ENABLEMENT_ANCHORS = [
  { key: "enablement", name: "默认开/关清单章节", needle: "默认开/关清单", alsoNeed: "当前状态" },
  { key: "enablement-recommend", name: "启用建议章节", needle: "启用建议" },
  { key: "enablement-acceptance", name: "验收说明章节", needle: "验收说明" },
];

function readIfExists(p) {
  try {
    return fs.readFileSync(p, "utf8");
  } catch (e) {
    return null;
  }
}

// 归一化：剔除全部空白并转小写，锚点匹配不受换行/缩进影响。
function norm(s) {
  return String(s || "").replace(/\s+/g, "").toLowerCase();
}

function checkSecurityMd() {
  const content = readIfExists(SECURITY_MD);
  if (content === null || content.trim() === "") {
    return [{ key: "file", name: "SECURITY.md（仓库根）", ok: false, detail: "文件缺失或为空" }];
  }
  const nc = norm(content);
  return REQUIRED_ANCHORS.map((a) => {
    const ok = nc.includes(norm(a.needle)) && (!a.alsoNeed || nc.includes(norm(a.alsoNeed)));
    const want = a.alsoNeed ? `${a.needle} / ${a.alsoNeed}` : a.needle;
    return {
      key: a.key,
      name: a.name,
      ok,
      detail: ok ? "锚点存在" : `缺少锚点: ${want}`,
    };
  });
}

function checkEnablementPlan() {
  const content = readIfExists(ENABLEMENT_PLAN_MD);
  if (content === null || content.trim() === "") {
    return [{ key: "enablement", name: "docs/security/enablement-plan.md", ok: false, detail: "文件缺失或为空" }];
  }
  const nc = norm(content);
  return ENABLEMENT_ANCHORS.map((a) => {
    const ok = nc.includes(norm(a.needle)) && (!a.alsoNeed || nc.includes(norm(a.alsoNeed)));
    const want = a.alsoNeed ? `${a.needle} / ${a.alsoNeed}` : a.needle;
    return {
      key: a.key,
      name: a.name,
      ok,
      detail: ok ? "锚点存在" : `缺少锚点: ${want}`,
    };
  });
}

function main() {
  const isCi = process.argv.includes("--ci");
  const results = [...checkSecurityMd(), ...checkEnablementPlan()];
  const failed = results.filter((r) => !r.ok);

  for (const r of results) {
    const flag = r.ok ? "PASS" : "FAIL";
    console.log(`SEC_AUDIT_${flag} check=${r.key} name=${r.name} ${r.detail}`);
  }

  if (failed.length === 0) {
    console.log("SECURITY_POLICY_GREEN total=ok 安全策略文件齐备");
    process.exitCode = 0;
  } else {
    console.log(
      `SECURITY_POLICY_RED total=${results.length} fail=${failed.length} ` +
        "缺文件/缺章节：需补 SECURITY.md 与 docs/security/enablement-plan.md"
    );
    process.exitCode = 1;
  }
}

main();
