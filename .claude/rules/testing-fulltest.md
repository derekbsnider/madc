# Testing: Use make fulltest

After making changes, always run `make -C src fulltest` to verify.
This runs both unit tests (doctest) and all integration tests in one command.
Do NOT run integration tests in a shell loop — use the Makefile target.
