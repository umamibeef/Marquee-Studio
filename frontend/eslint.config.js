// Lint for the front end. Enforces docs/PORTAL-PLAN.md §7: shadcn/ui exclusively, vendored in packages/ui.
import js from "@eslint/js"
import tseslint from "typescript-eslint"
import reactHooks from "eslint-plugin-react-hooks"
import globals from "globals"

// Other component libraries, and the primitives shadcn is built on (only packages/ui may use them).
const UI_LIBRARIES = [
  { group: ["@mui/*", "@material-ui/*", "@chakra-ui/*", "antd", "@ant-design/*", "@mantine/*", "react-bootstrap", "bootstrap", "@headlessui/*", "@ariakit/*", "react-aria", "react-aria-components", "@react-aria/*", "@nextui-org/*", "@heroui/*", "primereact", "semantic-ui-react", "@blueprintjs/*", "@fluentui/*", "evergreen-ui", "grommet", "rsuite", "@arco-design/*", "daisyui", "flowbite-react", "@ark-ui/*", "@base-ui-components/*", "emoji-mart", "@emoji-mart/*", "emoji-picker-react", "frimousse"], message: "shadcn/ui only (docs/PORTAL-PLAN.md §7): use @marquee/ui." },
  { group: ["@radix-ui/*", "radix-ui/*", "lucide-react/*"], message: "Import these through @marquee/ui (packages/ui), not directly." },
]
// Exact package names (a pattern would also match @marquee/ui/components/ui/sonner).
const VIA_UI = ["radix-ui", "cmdk", "vaul", "sonner", "next-themes", "lucide-react", "cn", "class-variance-authority"].map(
  (name) => ({ name, message: "Import this through @marquee/ui (packages/ui), not directly." }),
)

const RAW_ELEMENTS = "^(button|input|select|textarea|dialog)$"

export default tseslint.config(
  { ignores: ["**/dist/**", "**/node_modules/**", "scripts/**"] },
  js.configs.recommended,
  ...tseslint.configs.recommended,
  {
    files: ["**/*.{ts,tsx}"],
    languageOptions: { globals: { ...globals.browser }, ecmaVersion: 2023 },
    plugins: { "react-hooks": reactHooks },
    rules: {
      "react-hooks/rules-of-hooks": "error",
      "react-hooks/exhaustive-deps": "warn",
      "@typescript-eslint/no-unused-vars": ["error", { argsIgnorePattern: "^_", varsIgnorePattern: "^_", destructuredArrayIgnorePattern: "^_" }],
    },
  },
  {
    files: ["packages/ui/**/*.{ts,tsx}"],
    rules: { "react-hooks/exhaustive-deps": "off" },
  },
  {
    // Everything outside the vendored UI package: no other UI library, no Radix directly, no raw controls.
    files: ["apps/**/*.{ts,tsx}", "packages/sign-player/**/*.{ts,tsx}"],
    rules: {
      "no-restricted-imports": ["error", { paths: VIA_UI, patterns: UI_LIBRARIES }],
      "no-restricted-syntax": [
        "error",
        {
          selector: `JSXOpeningElement[name.name=/${RAW_ELEMENTS}/]`,
          message: "Use the shadcn component from @marquee/ui (Button, Input, Select, Textarea, Dialog).",
        },
        {
          selector: "JSXAttribute[name.name='role'][value.value=/^(button|tab|checkbox|radio|switch|slider|option|listbox|menuitem)$/]",
          message: "Don't fake a control with a role: use the shadcn primitive (Button, Tabs, Checkbox, RadioGroup, Switch, Slider, Select, Command, DropdownMenu).",
        },
      ],
    },
  },
  {
    files: ["apps/studio/vite.config.ts"],
    languageOptions: { globals: { ...globals.node } },
  },
)
