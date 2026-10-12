/**
 * Orlyscript syntax highlighter for the browser playground.
 * Maps keywords, built-ins, types, literals, and comments based on the TextMate grammar.
 */

const KEYWORDS = new Set([
  "package", "import", "where", "given", "effecting", "assert", "test",
  "if", "else", "else_if", "while", "start", "try",
  "keys", "take", "skip", "reduce", "sorted_by", "reversed", "filter", "split",
  "collated_by", "collected_by", "having", "that", "to", "from",
  "new", "mutate", "delete", "mut",
  "as", "is", "in", "opt", "empty", "free",
  "not", "xor", "known", "unknown", "true", "false"
]);

const TYPES = new Set([
  "bool", "int", "real", "str", "time_pnt", "time_diff", "id", "addr",
  "dict", "list", "set"
]);

export function highlightOrlyscript(code: string): string {
  const escapeHtml = (str: string) =>
    str.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");

  const tokens: string[] = [];
  let i = 0;
  const len = code.length;

  while (i < len) {
    // 1. Line comment
    if (code[i] === "/" && code[i + 1] === "/") {
      let end = code.indexOf("\n", i);
      if (end === -1) end = len;
      tokens.push(`<span class="tok-comment">${escapeHtml(code.slice(i, end))}</span>`);
      i = end;
      continue;
    }

    // 2. Block comment
    if (code[i] === "/" && code[i + 1] === "*") {
      let end = code.indexOf("*/", i + 2);
      if (end === -1) end = len; else end += 2;
      tokens.push(`<span class="tok-comment">${escapeHtml(code.slice(i, end))}</span>`);
      i = end;
      continue;
    }

    // 3. String literal
    if (code[i] === '"') {
      let end = i + 1;
      while (end < len) {
        if (code[end] === "\\") {
          end += 2;
        } else if (code[end] === '"') {
          end++;
          break;
        } else {
          end++;
        }
      }
      tokens.push(`<span class="tok-string">${escapeHtml(code.slice(i, end))}</span>`);
      i = end;
      continue;
    }

    // 4. Numbers
    if (/\d/.test(code[i]) && (i === 0 || !/[a-zA-Z0-9_]/.test(code[i - 1]))) {
      let end = i;
      while (end < len && /[0-9a-fA-FxX.]/.test(code[end])) {
        end++;
      }
      tokens.push(`<span class="tok-number">${escapeHtml(code.slice(i, end))}</span>`);
      i = end;
      continue;
    }

    // 5. Identifiers / Keywords / Types
    if (/[a-zA-Z_]/.test(code[i])) {
      let end = i;
      while (end < len && /[a-zA-Z0-9_]/.test(code[end])) {
        end++;
      }
      const word = code.slice(i, end);
      if (KEYWORDS.has(word)) {
        tokens.push(`<span class="tok-keyword">${escapeHtml(word)}</span>`);
      } else if (TYPES.has(word)) {
        tokens.push(`<span class="tok-type">${escapeHtml(word)}</span>`);
      } else {
        tokens.push(`<span class="tok-ident">${escapeHtml(word)}</span>`);
      }
      i = end;
      continue;
    }

    // 6. Named parameters .param:
    if (code[i] === "." && i + 1 < len && /[a-zA-Z_]/.test(code[i + 1])) {
      let end = i + 1;
      while (end < len && /[a-zA-Z0-9_]/.test(code[end])) {
        end++;
      }
      if (end < len && code[end] === ":") {
        tokens.push(`<span class="tok-param">${escapeHtml(code.slice(i, end + 1))}</span>`);
        i = end + 1;
        continue;
      }
    }

    // 7. Operators & Punctuation
    const op2 = code.slice(i, i + 2);
    if (["==", "!=", "<=", ">=", "+=", "-=", "*=", "/=", "|=", "::", "<-", "->"].includes(op2)) {
      tokens.push(`<span class="tok-operator">${escapeHtml(op2)}</span>`);
      i += 2;
      continue;
    }

    tokens.push(escapeHtml(code[i]));
    i++;
  }

  return tokens.join("");
}
