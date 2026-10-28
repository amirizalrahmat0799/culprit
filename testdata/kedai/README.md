A tiny sample project for testing `culprit investigate`: migration V7 adds a column that V5 already
added, which is what `../incident-migration.log` shows failing. The `.env` file holds a fake password, to
check that culprit never sends it.
