PLIST = $(HOME)/Library/LaunchAgents/tpgest.plist

tpgest: tpgest.c
	clang -O2 -Wall -Wextra -o $@ $< -framework ApplicationServices -framework CoreFoundation

# Run at login as a LaunchAgent, with this checkout's binary
install: tpgest
	sed -e 's|@BIN@|$(CURDIR)/tpgest|' -e 's|@LOG@|$(HOME)/Library/Logs/tpgest.log|' tpgest.plist.in > $(PLIST)
	launchctl bootstrap gui/$$(id -u) $(PLIST)

uninstall:
	-launchctl bootout gui/$$(id -u)/tpgest
	rm -f $(PLIST)

clean:
	rm -f tpgest

.PHONY: install uninstall clean
