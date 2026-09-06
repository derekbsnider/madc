// Shared adventure-pilot verb binding (Track 7.2 R1): the pilot's verbs
// are madc SOURCE in tests/adventure_verbs/<name>.madv — script-entity
// bindings attached through ui::bind_verb, gated by the world file's
// %verb declarations. Included by the driver and the affordance probe.
#include <cstdio>
#include <ns_ui>

static const char *ADV_VERBS[] = {
    "go", "take", "drop", "open", "light", "douse", "inventory", "edit"
};

void bind_adventure_verbs(long w)
{
    char path[256];
    char buf[8192];
    for ( int i = 0; i < 8; i = i + 1 )
    {
	snprintf(path, sizeof(path), "tests/adventure_verbs/%s.madv",
		 ADV_VERBS[i]);
	FILE *f = fopen(path, "r");
	if ( !f )
	{
	    printf("missing verb body: %s\n", path);
	    continue;
	}
	long n = fread(buf, 1, sizeof(buf) - 1, f);
	buf[n] = 0;
	fclose(f);
	ui::bind_verb(w, ADV_VERBS[i], buf);
    }
}
