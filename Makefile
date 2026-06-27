# Wikore convenience Makefile.
#
# Thin wrapper around the underlying tools so common workflows (build,
# test, corpus fetch) have memorable entrypoints. This is *not* the
# build system - CMake remains the source of truth for compilation.
#
# Usage: `make help`
#

.PHONY: help \
        corpus-negative corpus-fetch-100 corpus-fetch-3000 corpus-verify \
        corpus-test smoke

# Default Python is whatever is on PATH; override with PYTHON=python3.13 etc.
PYTHON ?= python3

# ===========================================================================
# Help
# ===========================================================================

help:
	@echo "Wikore make targets:"
	@echo ""
	@echo "  Test corpus (public document fetching + negative-sample generation)"
	@echo "    corpus-negative      Generate the 18 negative samples (~30 KB; gitignored)"
	@echo "    corpus-fetch-100     Fetch 100 docs from the positive seed manifest"
	@echo "    corpus-fetch-3000    Fetch 3000 docs (or all available; placeholder until"
	@echo "                         the source builders expand the manifest)"
	@echo "    corpus-verify        Re-generate negatives in memory and assert SHAs"
	@echo "                         match the manifest (CI determinism gate)"
	@echo "    corpus-test          Run tests/python/test_corpus.py (stdlib unittest)"
	@echo ""
	@echo "  Database"
	@echo "    smoke                Run db/smoke_test.sh against postgres:17 in docker"
	@echo ""

# ===========================================================================
# Test corpus
# ===========================================================================

# Generate the negative sample files into tests/fixtures/corpus/negative/
# (gitignored). Idempotent: re-running overwrites with the same bytes if
# the manifest's SHAs match the generator output.
corpus-negative:
	$(PYTHON) scripts/corpus/generate_negatives.py

# Determinism gate. Used in CI: regenerates every sample in memory and
# asserts the observed SHA-256 matches the value baked into the manifest.
# No files are written.
corpus-verify:
	$(PYTHON) scripts/corpus/generate_negatives.py --verify

# Fetch ~100 documents from the seed manifest. Suitable for per-iteration
# end-to-end tests; fits in a few minutes on a typical home connection.
corpus-fetch-100:
	$(PYTHON) scripts/corpus/fetch.py --limit 100

# Fetch 3000 documents. Today the seed manifest holds far fewer entries;
# extending it via the SEC EDGAR / arXiv / data.gov / EU portal builders
# (see tests/fixtures/corpus/README.md) is the natural follow-up.
corpus-fetch-3000:
	$(PYTHON) scripts/corpus/fetch.py --limit 3000

# Unit-test the corpus tooling itself: manifest schema, generator
# determinism, EICAR canonical SHA, fetcher filter / rate-limit logic.
# Stdlib unittest - no pytest required.
corpus-test:
	$(PYTHON) -m unittest discover -s tests/python -p 'test_*.py' -v

# ===========================================================================
# Database
# ===========================================================================

smoke:
	bash db/smoke_test.sh
