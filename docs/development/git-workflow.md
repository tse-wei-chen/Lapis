# Git workflow

Lapis uses `develop` as the integration branch and `master` as the release branch.

```text
feature/* ──PR──> develop ──release PR──> master
fix/*     ──PR──> develop
```

Rules:

1. Feature and normal fix branches are created from `develop` and merge back into `develop`.
2. `master` is not a daily-development target.
3. Before a release, stabilize on `develop`; the release change enters `master` through a `develop -> master` PR.
4. Hotfixes for an already released version may branch from `master`, but their final change must also be reconciled back into `develop`.
5. Keep feature PR history compact. Prefer amending a feature commit while fixing its own CI failures rather than producing a long series of mechanical `fix CI` commits.
6. Both managed and native CI must be green before a feature PR is merged into `develop`.
