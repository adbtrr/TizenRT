---
name: tizenrt-patch-workflow
description: Rules for creating a git patch from a requirement change and pushing it to a specified branch in TizenRT work. Use whenever the user asks to create a patch and push it to a named branch.
---

# TizenRT Patch & Push Workflow

Trigger: user says something like "create a new patch based on this requirement change and push it to branch <branch-name>".

Follow these rules exactly, every time, no exceptions.

## 1. No PR
- Never create, open, or suggest a pull request.
- Only ever push the branch itself.

## 2. No real email — use placeholder
- Author and committer email = `<branch-name>@local`
- If the branch name has slashes, replace them with `-` for the email
  (e.g. branch `feature/bl2-fix` → `feature-bl2-fix@local`)

## 3. Author name = Committer name = branch name
GIT_COMMITTER_NAME="<branch-name>" GIT_COMMITTER_EMAIL="<branch-name>@local" \
git commit --author="<branch-name> <<branch-name>@local>" -m "<message>"

## 4. No "Claude" anywhere
- Not in the commit message, commit body, code comments, or patch filename.
- Disable Claude Code's default trailer — do NOT add "Generated with Claude Code"
  or "Co-Authored-By: Claude" to any commit.

## 5. Always confirm before push — no standing permission
- Naming the branch in the request is NOT permission to push.
- After the commit/patch is created locally, stop and ask explicitly:
  "Ready to push to <branch-name>. Confirm push?"
- Only run `git push` after an explicit "yes" in that same turn.
- This applies even to branches Claude itself created.
