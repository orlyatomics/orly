.NOTPARALLEL:

# jhm itself lives in tools/ after bootstrap, and it spawns make_dep_file
# (also in tools/) as a subprocess. Put tools/ on PATH so every recipe
# can find both binaries by name -- matches how bootstrap.sh invokes them.
export PATH := $(CURDIR)/tools:$(PATH)

PREFIX ?= /usr/local
BINDIR=$(PREFIX)/bin
PACKAGE_DIR=$(PREFIX)/packages
DATA_DIR=$(PREFIX)/data
# jhm emits to ../out_orly/<config>/ (see README / docs/walkthrough.md), not ../out/.
RELEASE_OUT=../out_orly/release/
#Apps get installed on 'make install'
ORLY_APPS=orly/orlyc orly/server/orlyi orly/spa/spa orly/client/orly_client orly/indy/disk/util/orly_dm orly/core_import
ORLY_DATASET_GEN=beer complete_graph game_of_thrones money_laundering belgian_beer friends_of_friends matrix shakespeare social_graph twitter twitter_ego twitter_live
JHM_CMD=jhm $(JHM_FLAGS)
.PHONY: apps debug release bootstrap nycr test test_lang clean install

debug: bootstrap
	$(JHM_CMD)

release: bootstrap
	$(JHM_CMD) -c release

bootstrap: tools/jhm nycr

tools/jhm:
	./bootstrap.sh

nycr: tools/jhm
	$(JHM_CMD) -c bootstrap

test: bootstrap
	$(JHM_CMD) --test

test_lang: debug
	python3 tools/lang_test.py -d orly/data tests/lang_tests

clean:
	rm -rf ../out_orly/
	rm -rf ../.jhm
	rm -f tools/jhm
	rm -f tools/make_dep_file
	# jhm unions the per-job *.compdb.json into compile_commands.json at the
	# repo root (not under ../out_orly/), so wipe it too or clangd keeps
	# resolving against a stale, just-deleted build tree.
	rm -f compile_commands.json

install: release
	install -d $(BINDIR) $(PACKAGE_DIR) $(DATA_DIR)
	install -m755 -t $(BINDIR) $(addprefix $(RELEASE_OUT), $(ORLY_APPS))
	install -m755 -t $(BINDIR) $(addprefix $(RELEASE_OUT), $(addprefix orly/data/,$(ORLY_DATASET_GEN)))
	#Mark the root of the package repository
	touch $(PACKAGE_DIR)/__orly__
	install -m644 -t $(PACKAGE_DIR) $(wildcard orly/data/*.orly)
