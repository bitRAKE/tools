# Hype Blog

This folder contains adoption-focused HTML posts:

- `hype/index.html` (blog roll)
- `hype/<tool>.html` (per-tool post)

## GitHub Pages in the Same Repo

Yes, this blog can live in the same repository.

### Option A (simplest): serve from repository root

1. Push `hype/` to `main`.
2. GitHub: **Settings → Pages**
3. **Build and deployment**: `Deploy from a branch`
4. **Branch**: `main`, **Folder**: `/(root)`
5. Open: `https://<owner>.github.io/<repo>/hype/`

### Option B: serve from `/docs`

Use this if you want to isolate published content from repo root.

1. Move/copy blog files to `docs/`.
2. GitHub: **Settings → Pages**
3. Source: `main` + `/docs`
4. Open: `https://<owner>.github.io/<repo>/`

### Option C: deploy via GitHub Actions

Use this when you want generated/processed pages and a strict publish pipeline.

---

## Authoring Rules

- Dates should reflect the tool's latest commit date, not post creation date.
- Avoid local identifying paths in snippets.
- Keep claims aligned with tested behavior.
