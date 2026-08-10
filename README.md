# Dead Trigger — NextOS

Private source and release repository for the universal AArch64 NextOS /
PortMaster compatibility port of **Dead Trigger 1 v2.1.0**.

The repository contains only open-source compatibility code, shared loader
primitives, framework components, manifests, tests and the BYO-data installer.
It does not contain the APK, Android/Unity libraries, game assets, artwork,
audio, video or saves.

The release uses a self-contained nxbootstrap 0.6 launcher, NXExtract 1.2.6,
capability-driven nxcompat receipts and a reproducible GLIBC 2.17 AArch64
build. Clean extraction UI, video, nonzero PCM audio, controller input and
rendered gameplay were validated on NextOS Mali-450, X5M and Ark families.

See `ports/deadtrigger/README.md` and `INSTALLATION.md` for architecture,
controls, supported owner payload and build instructions. The BYO-data ZIP is
published only through the GitHub Releases page.

Canonical monorepo source commit:
`3ed8f2bb2e9029c2c6ae7eeac07c8d6f25a9e025`.
