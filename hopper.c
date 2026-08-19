/*
 * tmux-hopper: YAML-driven session builder for tmux.
 *
 * Parses a hopper.yaml file describing named sessions (or templated
 * "types" plus a runtime value) and turns them into a batch of standard
 * tmux commands (new-session -d, new-window, send-keys ...) which are
 * spliced into the command queue.
 *
 * All user-supplied strings are single-quoted before being handed to
 * the tmux parser, so shell metacharacters in ssh hosts or commands are
 * safe.
 */

#include <sys/types.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "compat/yaml/yaml.h"
#include "tmux.h"
#include "hopper.h"

TAILQ_HEAD(hopper_strings, hopper_string);
struct hopper_string {
	char			*text;
	TAILQ_ENTRY(hopper_string) entry;
};

TAILQ_HEAD(hopper_windows, hopper_window);
struct hopper_window {
	char			*name;		/* may contain {{value}} */
	char			*ssh;		/* may be NULL */
	struct hopper_strings	 commands;	/* each may contain {{value}} */
	TAILQ_ENTRY(hopper_window) entry;
};

TAILQ_HEAD(hopper_sessions, hopper_session);
struct hopper_session {
	char			*name;		/* NULL if type-based */
	char			*type;		/* NULL if name-based */
	struct hopper_windows	 windows;
	TAILQ_ENTRY(hopper_session) entry;
};

struct hopper_config {
	struct hopper_sessions	 sessions;
};

static struct hopper_config	*hopper_cache;
static int			 hopper_cache_valid;

/* ---- utility ---------------------------------------------------------- */

static void printflike(1, 2)
hopper_warn(const char *fmt, ...)
{
	va_list	ap;

	va_start(ap, fmt);
	fputs("tmux-hopper: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static char *
hopper_config_path(void)
{
	const char	*env;
	const char	*home;
	char		*path;

	env = getenv("TMUX_HOPPER_CONFIG");
	if (env != NULL && *env != '\0')
		return (xstrdup(env));

	env = getenv("XDG_CONFIG_HOME");
	if (env != NULL && *env != '\0') {
		xasprintf(&path, "%s/tmux-hopper/hopper.yaml", env);
		return (path);
	}

	home = getenv("HOME");
	if (home == NULL || *home == '\0')
		return (NULL);
	xasprintf(&path, "%s/.config/tmux-hopper/hopper.yaml", home);
	return (path);
}

/*
 * Replace occurrences of "{{value}}" in `in` with `value`. Always
 * returns a newly-allocated string (even when no substitution happens),
 * so the caller can free it unconditionally. If `value` is NULL and the
 * placeholder is present the placeholder is left in place (caller will
 * have already validated we have a value when needed).
 */
static char *
hopper_expand(const char *in, const char *value)
{
	static const char	 needle[] = "{{value}}";
	const size_t		 nlen = sizeof(needle) - 1;
	const char		*p, *hit;
	char			*out;
	size_t			 vlen, outlen, outcap;

	if (in == NULL)
		return (NULL);
	if (value == NULL || strstr(in, needle) == NULL)
		return (xstrdup(in));

	vlen = strlen(value);
	outcap = strlen(in) + 1;
	out = xmalloc(outcap);
	outlen = 0;

	p = in;
	while ((hit = strstr(p, needle)) != NULL) {
		size_t chunk = (size_t)(hit - p);
		while (outlen + chunk + vlen + 1 > outcap) {
			outcap *= 2;
			out = xrealloc(out, outcap);
		}
		memcpy(out + outlen, p, chunk);
		outlen += chunk;
		memcpy(out + outlen, value, vlen);
		outlen += vlen;
		p = hit + nlen;
	}
	{
		size_t tail = strlen(p);
		while (outlen + tail + 1 > outcap) {
			outcap *= 2;
			out = xrealloc(out, outcap);
		}
		memcpy(out + outlen, p, tail);
		outlen += tail;
	}
	out[outlen] = '\0';
	return (out);
}

/*
 * Wrap `s` in single quotes for the tmux command parser, escaping any
 * embedded single quotes as '\''. Returns a newly-allocated string.
 */
static char *
hopper_shquote(const char *s)
{
	const char	*p;
	char		*out;
	size_t		 cap, i;

	if (s == NULL)
		s = "";
	cap = strlen(s) * 4 + 3;
	out = xmalloc(cap);
	i = 0;
	out[i++] = '\'';
	for (p = s; *p != '\0'; p++) {
		if (*p == '\'') {
			out[i++] = '\'';
			out[i++] = '\\';
			out[i++] = '\'';
			out[i++] = '\'';
		} else {
			out[i++] = *p;
		}
	}
	out[i++] = '\'';
	out[i] = '\0';
	return (out);
}

/* ---- YAML parsing ---------------------------------------------------- */

/*
 * The YAML we accept is small and rigid:
 *
 *   sessions:
 *     - name: STR                 (or)   type: STR
 *       windows:
 *         - name: STR
 *           ssh: STR              (optional)
 *           commands:             (optional; scalar or list of scalars)
 *             - STR
 *             - STR
 *
 * Rather than an event-tree walker we use a small state machine over
 * libyaml's event stream. Unexpected structure is reported as a soft
 * error and the whole config is discarded.
 */

static int
hopper_parse_commands(yaml_parser_t *p, struct hopper_strings *out)
{
	yaml_event_t	 ev;
	struct hopper_string *cs;
	int		 depth = 1;

	TAILQ_INIT(out);
	for (;;) {
		if (!yaml_parser_parse(p, &ev))
			return (-1);
		if (ev.type == YAML_SEQUENCE_START_EVENT) {
			depth++;
		} else if (ev.type == YAML_SEQUENCE_END_EVENT) {
			depth--;
			yaml_event_delete(&ev);
			if (depth == 0)
				return (0);
			continue;
		} else if (ev.type == YAML_SCALAR_EVENT) {
			cs = xcalloc(1, sizeof *cs);
			cs->text = xstrdup((char *)ev.data.scalar.value);
			TAILQ_INSERT_TAIL(out, cs, entry);
		} else if (ev.type == YAML_STREAM_END_EVENT) {
			yaml_event_delete(&ev);
			return (-1);
		}
		yaml_event_delete(&ev);
	}
}

static int
hopper_parse_window(yaml_parser_t *p, struct hopper_window *w)
{
	yaml_event_t	 ev;
	char		*key = NULL;
	int		 rc = 0;

	TAILQ_INIT(&w->commands);
	for (;;) {
		if (!yaml_parser_parse(p, &ev))
			return (-1);
		if (ev.type == YAML_MAPPING_END_EVENT) {
			yaml_event_delete(&ev);
			free(key);
			return (rc);
		}
		if (key == NULL && ev.type != YAML_SCALAR_EVENT) {
			yaml_event_delete(&ev);
			free(key);
			return (-1);
		}
		if (key == NULL) {
			key = xstrdup((char *)ev.data.scalar.value);
			yaml_event_delete(&ev);
			continue;
		}
		/* value event for `key` */
		if (strcmp(key, "name") == 0) {
			free(w->name);
			w->name = xstrdup((char *)ev.data.scalar.value);
			yaml_event_delete(&ev);
		} else if (strcmp(key, "ssh") == 0) {
			free(w->ssh);
			w->ssh = xstrdup((char *)ev.data.scalar.value);
			yaml_event_delete(&ev);
		} else if (strcmp(key, "commands") == 0) {
			if (ev.type == YAML_SCALAR_EVENT) {
				/* single-scalar shorthand */
				struct hopper_string *cs;
				cs = xcalloc(1, sizeof *cs);
				cs->text = xstrdup((char *)ev.data.scalar.value);
				TAILQ_INSERT_TAIL(&w->commands, cs, entry);
				yaml_event_delete(&ev);
			} else {
				/*
				 * Expect a sequence-start (which we already
				 * consumed here as `ev`).  If not a sequence,
				 * bail.
				 */
				int seq = (ev.type == YAML_SEQUENCE_START_EVENT);
				yaml_event_delete(&ev);
				if (!seq) { rc = -1; }
				else if (hopper_parse_commands(p, &w->commands) != 0)
					rc = -1;
			}
		} else {
			/* unknown key: skip its value */
			yaml_event_delete(&ev);
		}
		free(key);
		key = NULL;
	}
}

static int
hopper_parse_windows(yaml_parser_t *p, struct hopper_windows *out)
{
	yaml_event_t		 ev;
	struct hopper_window	*w;

	TAILQ_INIT(out);
	for (;;) {
		if (!yaml_parser_parse(p, &ev))
			return (-1);
		if (ev.type == YAML_SEQUENCE_END_EVENT) {
			yaml_event_delete(&ev);
			return (0);
		}
		if (ev.type != YAML_MAPPING_START_EVENT) {
			yaml_event_delete(&ev);
			return (-1);
		}
		yaml_event_delete(&ev);
		w = xcalloc(1, sizeof *w);
		if (hopper_parse_window(p, w) != 0) {
			free(w->name);
			free(w->ssh);
			free(w);
			return (-1);
		}
		TAILQ_INSERT_TAIL(out, w, entry);
	}
}

static int
hopper_parse_session(yaml_parser_t *p, struct hopper_session *s)
{
	yaml_event_t	 ev;
	char		*key = NULL;
	int		 rc = 0;

	TAILQ_INIT(&s->windows);
	for (;;) {
		if (!yaml_parser_parse(p, &ev))
			return (-1);
		if (ev.type == YAML_MAPPING_END_EVENT) {
			yaml_event_delete(&ev);
			free(key);
			return (rc);
		}
		if (key == NULL && ev.type != YAML_SCALAR_EVENT) {
			yaml_event_delete(&ev);
			free(key);
			return (-1);
		}
		if (key == NULL) {
			key = xstrdup((char *)ev.data.scalar.value);
			yaml_event_delete(&ev);
			continue;
		}
		if (strcmp(key, "name") == 0 &&
		    ev.type == YAML_SCALAR_EVENT) {
			free(s->name);
			s->name = xstrdup((char *)ev.data.scalar.value);
			yaml_event_delete(&ev);
		} else if (strcmp(key, "type") == 0 &&
		    ev.type == YAML_SCALAR_EVENT) {
			free(s->type);
			s->type = xstrdup((char *)ev.data.scalar.value);
			yaml_event_delete(&ev);
		} else if (strcmp(key, "windows") == 0) {
			int seq = (ev.type == YAML_SEQUENCE_START_EVENT);
			yaml_event_delete(&ev);
			if (!seq) rc = -1;
			else if (hopper_parse_windows(p, &s->windows) != 0)
				rc = -1;
		} else {
			yaml_event_delete(&ev);
		}
		free(key);
		key = NULL;
	}
}

static int
hopper_parse_sessions(yaml_parser_t *p, struct hopper_sessions *out)
{
	yaml_event_t		 ev;
	struct hopper_session	*s;

	TAILQ_INIT(out);
	for (;;) {
		if (!yaml_parser_parse(p, &ev))
			return (-1);
		if (ev.type == YAML_SEQUENCE_END_EVENT) {
			yaml_event_delete(&ev);
			return (0);
		}
		if (ev.type != YAML_MAPPING_START_EVENT) {
			yaml_event_delete(&ev);
			return (-1);
		}
		yaml_event_delete(&ev);
		s = xcalloc(1, sizeof *s);
		if (hopper_parse_session(p, s) != 0) {
			free(s->name);
			free(s->type);
			free(s);
			return (-1);
		}
		TAILQ_INSERT_TAIL(out, s, entry);
	}
}

static int
hopper_parse_root(yaml_parser_t *p, struct hopper_config *cfg)
{
	yaml_event_t	 ev;
	char		*key = NULL;
	int		 in_doc = 0, in_map = 0;

	for (;;) {
		if (!yaml_parser_parse(p, &ev))
			return (-1);
		switch (ev.type) {
		case YAML_STREAM_START_EVENT:
			break;
		case YAML_STREAM_END_EVENT:
			yaml_event_delete(&ev);
			return (0);
		case YAML_DOCUMENT_START_EVENT:
			in_doc = 1;
			break;
		case YAML_DOCUMENT_END_EVENT:
			in_doc = 0;
			in_map = 0;
			break;
		case YAML_MAPPING_START_EVENT:
			if (!in_doc) { yaml_event_delete(&ev); return (-1); }
			in_map = 1;
			break;
		case YAML_MAPPING_END_EVENT:
			in_map = 0;
			free(key);
			key = NULL;
			break;
		case YAML_SCALAR_EVENT:
			if (!in_map) { yaml_event_delete(&ev); return (-1); }
			if (key == NULL) {
				key = xstrdup((char *)ev.data.scalar.value);
			} else {
				/* value was a scalar for an unknown key */
				free(key);
				key = NULL;
			}
			break;
		case YAML_SEQUENCE_START_EVENT:
			if (key != NULL && strcmp(key, "sessions") == 0) {
				yaml_event_delete(&ev);
				if (hopper_parse_sessions(p, &cfg->sessions) != 0) {
					free(key);
					return (-1);
				}
				free(key);
				key = NULL;
				continue;
			}
			/* unknown sequence; skip it */
			{
				int depth = 1;
				yaml_event_delete(&ev);
				while (depth > 0) {
					if (!yaml_parser_parse(p, &ev))
						{ free(key); return (-1); }
					if (ev.type == YAML_SEQUENCE_START_EVENT ||
					    ev.type == YAML_MAPPING_START_EVENT)
						depth++;
					else if (ev.type == YAML_SEQUENCE_END_EVENT ||
					    ev.type == YAML_MAPPING_END_EVENT)
						depth--;
					yaml_event_delete(&ev);
				}
				free(key);
				key = NULL;
				continue;
			}
		default:
			break;
		}
		yaml_event_delete(&ev);
	}
}

static void
hopper_free_config(struct hopper_config *cfg)
{
	struct hopper_session	*s;
	struct hopper_window	*w;
	struct hopper_string	*cs;

	if (cfg == NULL)
		return;
	while ((s = TAILQ_FIRST(&cfg->sessions)) != NULL) {
		TAILQ_REMOVE(&cfg->sessions, s, entry);
		while ((w = TAILQ_FIRST(&s->windows)) != NULL) {
			TAILQ_REMOVE(&s->windows, w, entry);
			while ((cs = TAILQ_FIRST(&w->commands)) != NULL) {
				TAILQ_REMOVE(&w->commands, cs, entry);
				free(cs->text);
				free(cs);
			}
			free(w->name);
			free(w->ssh);
			free(w);
		}
		free(s->name);
		free(s->type);
		free(s);
	}
	free(cfg);
}

/* Returns cached config, or NULL if unavailable. Soft error path. */
static struct hopper_config *
hopper_load(void)
{
	struct hopper_config	*cfg;
	FILE			*f;
	yaml_parser_t		 parser;
	char			*path;

	if (hopper_cache_valid)
		return (hopper_cache);
	hopper_cache_valid = 1;

	path = hopper_config_path();
	if (path == NULL)
		return (NULL);
	f = fopen(path, "r");
	if (f == NULL) {
		if (errno != ENOENT)
			hopper_warn("cannot open %s: %s", path, strerror(errno));
		free(path);
		return (NULL);
	}

	if (!yaml_parser_initialize(&parser)) {
		hopper_warn("yaml_parser_initialize failed");
		fclose(f);
		free(path);
		return (NULL);
	}
	yaml_parser_set_input_file(&parser, f);

	cfg = xcalloc(1, sizeof *cfg);
	TAILQ_INIT(&cfg->sessions);
	if (hopper_parse_root(&parser, cfg) != 0) {
		hopper_warn("failed to parse %s (line %zu)", path,
		    (size_t)parser.problem_mark.line + 1);
		hopper_free_config(cfg);
		cfg = NULL;
	}
	yaml_parser_delete(&parser);
	fclose(f);
	free(path);

	hopper_cache = cfg;
	return (cfg);
}

/* ---- lookup and dispatch --------------------------------------------- */

/*
 * Locate a session spec matching (argv0[, argv1]). Sets *out_value to
 * argv1 when the match is a type-based (templated) session, or NULL
 * otherwise.
 */
static struct hopper_session *
hopper_lookup(struct hopper_config *cfg, const char *argv0, const char *argv1,
    const char **out_value)
{
	struct hopper_session	*s;

	*out_value = NULL;
	if (cfg == NULL || argv0 == NULL)
		return (NULL);

	TAILQ_FOREACH(s, &cfg->sessions, entry) {
		if (s->name != NULL && strcmp(s->name, argv0) == 0)
			return (s);
	}
	if (argv1 == NULL)
		return (NULL);
	TAILQ_FOREACH(s, &cfg->sessions, entry) {
		if (s->type != NULL && strcmp(s->type, argv0) == 0) {
			*out_value = argv1;
			return (s);
		}
	}
	return (NULL);
}

/*
 * Build the tmux command block that materialises `s` and splice it into
 * the command queue after `item`. Returns 0 on success, -1 on error
 * (already reported via cmdq_error).
 */
static int
hopper_dispatch(struct cmdq_item *item, struct hopper_session *s,
    const char *value, int detached)
{
	struct evbuffer		*buf;
	struct hopper_window	*w;
	struct hopper_string	*cs;
	struct cmd_parse_input	 pi;
	char			*sname_raw, *sname_q, *wname_raw, *wname_q;
	char			*txt_raw, *txt_q;
	char			*cause = NULL;
	int			 first = 1, rc = 0;
	enum cmd_parse_status	 st;

	buf = evbuffer_new();
	if (buf == NULL)
		fatal("evbuffer_new");

	/* Session name: use configured name, or "<type>-<value>" for types. */
	if (s->name != NULL)
		sname_raw = xstrdup(s->name);
	else
		xasprintf(&sname_raw, "%s-%s", s->type,
		    value != NULL ? value : "session");
	sname_q = hopper_shquote(sname_raw);

	TAILQ_FOREACH(w, &s->windows, entry) {
		wname_raw = hopper_expand(w->name != NULL ? w->name : "",
		    value);
		wname_q = hopper_shquote(wname_raw);

		if (first) {
			evbuffer_add_printf(buf,
			    "new-session -d -s %s -n %s\n",
			    sname_q, wname_q);
			first = 0;
		} else {
			evbuffer_add_printf(buf,
			    "new-window -t %s: -n %s\n",
			    sname_q, wname_q);
		}

		/* ssh: is sugar for a leading `ssh <host>` command. */
		if (w->ssh != NULL && *w->ssh != '\0') {
			char *host_exp, *full;

			host_exp = hopper_expand(w->ssh, value);
			xasprintf(&full, "ssh %s", host_exp);
			txt_q = hopper_shquote(full);
			evbuffer_add_printf(buf,
			    "send-keys -t %s:%s %s Enter\n",
			    sname_q, wname_q, txt_q);
			free(host_exp);
			free(full);
			free(txt_q);
		}

		TAILQ_FOREACH(cs, &w->commands, entry) {
			txt_raw = hopper_expand(cs->text, value);
			txt_q = hopper_shquote(txt_raw);
			evbuffer_add_printf(buf,
			    "send-keys -t %s:%s %s Enter\n",
			    sname_q, wname_q, txt_q);
			free(txt_raw);
			free(txt_q);
		}

		free(wname_raw);
		free(wname_q);
	}

	if (first) {
		/* No windows in spec — fall through so stock behavior runs. */
		free(sname_raw);
		free(sname_q);
		evbuffer_free(buf);
		cmdq_error(item, "hopper: session '%s' has no windows",
		    s->name != NULL ? s->name : s->type);
		return (-1);
	}

	/* Attach or switch as appropriate. */
	if (!detached) {
		if (cmdq_get_client(item) != NULL &&
		    cmdq_get_client(item)->session != NULL) {
			evbuffer_add_printf(buf,
			    "switch-client -t %s\n", sname_q);
		} else {
			evbuffer_add_printf(buf,
			    "attach-session -t %s\n", sname_q);
		}
	}

	/* Splice into queue. */
	memset(&pi, 0, sizeof pi);
	pi.item = item;
	pi.c = cmdq_get_client(item);
	pi.file = "<tmux-hopper>";
	pi.line = 1;

	{
		const char *src = (const char *)EVBUFFER_DATA(buf);
		char *nul_terminated = xstrndup(src, EVBUFFER_LENGTH(buf));
		enum cmd_parse_status	st2;
		st2 = cmd_parse_and_insert(nul_terminated, &pi, item,
		    cmdq_get_state(item), &cause);
		free(nul_terminated);
		st = st2;
	}
	if (st == CMD_PARSE_ERROR) {
		cmdq_error(item, "hopper: %s",
		    cause != NULL ? cause : "parse failed");
		free(cause);
		rc = -1;
	}

	free(sname_raw);
	free(sname_q);
	evbuffer_free(buf);
	return (rc);
}

int
hopper_new_session_hook(struct cmd *self, struct cmdq_item *item, int detached)
{
	struct hopper_config	*cfg;
	struct hopper_session	*match;
	const char		*value = NULL;
	const char		*a0, *a1;
	struct args		*args;
	u_int			 count;

	args = cmd_get_args(self);
	if (args == NULL)
		return (0);

	/* Only trigger when at least one positional arg present and no -t. */
	if (args_has(args, 't') || args_has(args, 's'))
		return (0);
	count = args_count(args);
	if (count == 0)
		return (0);

	a0 = args_string(args, 0);
	a1 = count > 1 ? args_string(args, 1) : NULL;
	if (a0 == NULL || *a0 == '\0')
		return (0);

	cfg = hopper_load();
	if (cfg == NULL)
		return (0);

	match = hopper_lookup(cfg, a0, a1, &value);
	if (match == NULL)
		return (0);

	if (hopper_dispatch(item, match, value, detached) != 0)
		return (-1);
	return (1);
}
