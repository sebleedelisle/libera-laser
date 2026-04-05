# Repo Instructions

## C++ Style
- Match the naming style already used in nearby code before introducing new identifiers.
- Do not introduce `k`-prefixed constants. For local or file-scope constants, prefer descriptive `lowerCamelCase` names such as `maxHeliosNameLength`. For constants that are settings at a global or class level static, use UPPER_SNAKE_CASE 
- Do not use trailing underscores on member variables (no `socket_`, `io_`, etc.). Member variables are plain `lowerCamelCase`. If a member collides with an accessor name, rename the accessor to `getXxx()` / `setXxx()` rather than decorating the member.
- Avoid style-only churn outside the code needed for the task.
- Always add easily readable and understandable comments to explain strategies and aims. If you have to use deep C++ jargon / patterns please also explain them the first time.
- If a line of code isn't easily understandable add a comment above it to make it clearer. 
