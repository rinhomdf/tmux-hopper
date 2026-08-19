/*
 * tmux-hopper — YAML-driven session/window layouts with SSH chaining.
 *
 * See example.hopper.yaml for schema.
 */

#ifndef HOPPER_H
#define HOPPER_H

struct cmd;
struct cmdq_item;

/*
 * Called from cmd_new_session_exec before the stock path runs.
 *
 * Looks up positional arguments (name, or type+value) in the user's
 * hopper YAML file. If a match is found, this function synthesises a
 * batch of tmux commands (new-session -d, send-keys, new-window,
 * optional attach/switch) and splices them into the command queue
 * after `item`, then returns 1 so the caller returns CMD_RETURN_NORMAL
 * without executing the original new-session logic.
 *
 * Returns:
 *   1 = dispatched, caller must return CMD_RETURN_NORMAL immediately.
 *   0 = no match, caller should fall through to stock behavior.
 *  -1 = matched but dispatch failed (error already reported via cmdq_error);
 *       caller should return CMD_RETURN_ERROR.
 *
 * Soft-fails (missing/malformed config, unknown positional arg) fall
 * through with 0 after logging to stderr / server log.
 */
int	hopper_new_session_hook(struct cmd *self, struct cmdq_item *item,
	    int detached);

#endif /* HOPPER_H */
