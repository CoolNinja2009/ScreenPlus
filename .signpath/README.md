# Code signing (SignPath Foundation)

Releases are signed for free by SignPath Foundation, so Windows Defender and SmartScreen stop
flagging Screen+ as an unknown unsigned program. Until it's set up, the workflow skips signing.

1. Apply at https://signpath.org/apply with the GitHub repository. The project must be public and
   MIT-licensed, and it must have a code signing policy on its website or README (see below).
2. Once you're accepted, open the SignPath web app:
   - Create the project with the slug `ScreenPlus` and link it to this repository (GitHub trusted build system).
   - Add the artifact configurations `binaries` and `installer`, pasting the XML from
     `artifact-configurations/`.
   - Use the `release-signing` signing policy (each release waits for your approval in SignPath).
   - Create a CI user, copy its API token, and give it submitter rights on the policy.
3. In GitHub, go to Settings → Secrets and variables → Actions:
   - Add the secret `SIGNPATH_API_TOKEN` (the CI user's token).
   - Add the variable `SIGNPATH_ORGANIZATION_ID` (from SignPath's organization settings).

The next push to main signs `ScreenPlus.exe` and `ScreenPlus.com`, rebuilds setup around them, then
signs setup. Approve both requests in SignPath, or within an hour the job times out.

README text SignPath asks for:

> Free code signing provided by [SignPath.io](https://about.signpath.io), certificate by
> [SignPath Foundation](https://signpath.org).
