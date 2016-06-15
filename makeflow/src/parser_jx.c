/*
Copyright (C) 2016- The University of Notre Dame
This software is distributed under the GNU General Public License.
See the file COPYING for details.
*/

#include "dag.h"
#include "dag_node.h"
#include "dag_variable.h"
#include "dag_resources.h"
#include "parser_jx.h"
#include "xxmalloc.h"
#include "set.h"
#include "itable.h"
#include "stringtools.h"
#include "path.h"
#include "hash_table.h"
#include "debug.h"
#include "parser.h"
#include "jx_eval.h"
#include "jx_match.h"

static int environment_from_jx(struct dag *d, struct dag_node *n, struct hash_table *h, struct jx *env) {
	debug(D_MAKEFLOW_PARSER, "Parsing environment");
	int nodeid;
	if (n) {
		nodeid = n->nodeid;
	} else {
		nodeid = 0;
	}

	if (jx_istype(env, JX_OBJECT)) {
		struct jx *item;
		void *i = NULL;
		while ((item = jx_iterate_keys(env, &i))) {
			char *key;
			const char *value;
			if (jx_match_string(item, &key)) {
				debug(D_MAKEFLOW_PARSER, "export %s", key);
				if ((value = jx_lookup_string(env, key))) {
					debug(D_MAKEFLOW_PARSER, "env %s=%s", key, value);
					dag_variable_add_value(key, h, nodeid, value);
				}
				set_insert(d->export_vars, key);
			} else {
				debug(D_MAKEFLOW_PARSER, "Environment key/value must be strings");
				return 0;
			}
		}
	} else {
		debug(D_MAKEFLOW_PARSER, "Expected environment to be an object");
		return 0;
	}
	return 1;
}

static int resources_from_jx(struct hash_table *h, struct jx *j) {
	debug(D_MAKEFLOW_PARSER, "Parsing resources");
	int cores = jx_lookup_integer(j, "cores");
	if (cores) {
		debug(D_MAKEFLOW_PARSER, "%d core(s)", cores);
		dag_variable_add_value(RESOURCES_CORES, h, 0, string_format("%d", cores));
	} else {
		debug(D_MAKEFLOW_PARSER, "cores malformed or missing");
	}

	int disk = jx_lookup_integer(j, "disk");
	if (disk) {
		debug(D_MAKEFLOW_PARSER, "%d disk(s)", disk);
		dag_variable_add_value(RESOURCES_DISK, h, 0, string_format("%d", disk));
	} else {
		debug(D_MAKEFLOW_PARSER, "disks malformed or missing");
	}

	int memory = jx_lookup_integer(j, "memory");
	if (memory) {
		debug(D_MAKEFLOW_PARSER, "%d memory", memory);
		dag_variable_add_value(RESOURCES_MEMORY, h, 0, string_format("%d", memory));
	} else {
		debug(D_MAKEFLOW_PARSER, "memory malformed or missing");
	}

	int gpus = jx_lookup_integer(j, "gpus");
	if (gpus) {
		debug(D_MAKEFLOW_PARSER, "%d gpus", gpus);
		dag_variable_add_value(RESOURCES_GPUS, h, 0, string_format("%d", gpus));
	} else {
		debug(D_MAKEFLOW_PARSER, "gpus malformed or missing");
	}

	return 1;
}

static int file_from_jx(struct dag_node *n, int input, struct jx *j) {
	char *path;
	if (jx_match_string(j, &path)) {
		if (input) {
			debug(D_MAKEFLOW_PARSER, "Input %s", path);
			dag_node_add_source_file(n, path, NULL);
		} else {
			debug(D_MAKEFLOW_PARSER, "Output %s", path);
			dag_node_add_target_file(n, path, NULL);
		}
		free(path);
		return 1;
	} else if (jx_istype(j, JX_OBJECT)) {
		const char *name = jx_lookup_string(j, "name");
		const char *source = jx_lookup_string(j, "source");
		if (!name) {
			debug(D_MAKEFLOW_PARSER, "File lacks a name");
			return 0;
		}
		if (input) {
			debug(D_MAKEFLOW_PARSER, "Input %s, remote name %s", name, source ? source : "NULL");
			dag_node_add_source_file(n, name, source);
		} else {
			debug(D_MAKEFLOW_PARSER, "Output %s, remote name %s", name, source ? source : "NULL");
			dag_node_add_target_file(n, name, source);
		}
		return 1;
	} else {
		debug(D_MAKEFLOW_PARSER, "Input must be a string or object");
		return 0;
	}
}

static int files_from_jx(struct dag_node *n, int inputs, struct jx *j) {
	if (jx_istype(j, JX_ARRAY)) {
		struct jx *item;
		void *i = NULL;
		while ((item = jx_iterate_array(j, &i))) {
			if (!file_from_jx(n, inputs, item)) {
				return 0;
			}
		}
		return 1;
	} else {
		debug(D_MAKEFLOW_PARSER, "files malformed or missing");
		return 1;
	}
}

static int rule_from_jx(struct dag *d, struct jx *context, struct jx *j) {
	debug(D_MAKEFLOW_PARSER, "Parsing rule");
	struct dag_node *n = dag_node_create(d, 0);
	context = jx_copy(context);
	jx_insert_integer(context, "RULENO", n->nodeid);

	struct jx *inputs = jx_eval(jx_lookup(j, "inputs"), context);
	debug(D_MAKEFLOW_PARSER, "Parsing inputs");
	if (!files_from_jx(n, 1, inputs)) {
		debug(D_MAKEFLOW_PARSER, "Failure parsing inputs");
		return 0;
	}
	struct jx *outputs = jx_eval(jx_lookup(j, "outputs"), context);
	debug(D_MAKEFLOW_PARSER, "Parsing outputs");
	if (!files_from_jx(n, 0, outputs)) {
		debug(D_MAKEFLOW_PARSER, "Failure parsing outputs");
		return 0;
	}

	jx_insert(context, jx_string("INPUTS"), inputs);
	jx_insert(context, jx_string("OUTPUTS"), outputs);

	struct jx *makeflow = jx_eval(jx_lookup(j, "makeflow"), context);
	struct jx *command = jx_eval(jx_lookup(j, "command"), context);

	if (makeflow && command) {
		debug(D_MAKEFLOW_PARSER, "Rule must not have both command and submakeflow");
		return 0;
	}

	if (jx_match_string(command, (char **) &n->command)) {
		debug(D_MAKEFLOW_PARSER, "command: %s", n->command);
	} else if (jx_istype(makeflow, JX_OBJECT)) {
		const char *path = jx_lookup_string(makeflow, "path");
		if (path) {
			debug(D_MAKEFLOW_PARSER, "Submakeflow at %s", path);
			n->nested_job = 1;
			n->makeflow_dag = xxstrdup(path);
			const char *cwd = jx_lookup_string(makeflow, "cwd");
			if (cwd) {
				debug(D_MAKEFLOW_PARSER, "working directory %s", cwd);
				n->makeflow_cwd = xxstrdup(cwd);
			} else {
				debug(D_MAKEFLOW_PARSER, "cwd malformed or missing, using process cwd");
				n->makeflow_cwd = path_getcwd();
			}
		} else {
			debug(D_MAKEFLOW_PARSER, "Submakeflow must specify a path");
			return 0;
		}
	} else {
		debug(D_MAKEFLOW_PARSER, "Rule must have a command or submakeflow");
		return 0;
	}
	n->next = d->nodes;
	d->nodes = n;
	itable_insert(d->node_table, n->nodeid, n);
	jx_delete(makeflow);
	jx_delete(command);

	struct jx *local_job = jx_eval(jx_lookup(j, "local_job"), context);
	int l;
	if (jx_match_boolean(local_job, &l) && l) {
		debug(D_MAKEFLOW_PARSER, "Local job");
		n->local_job = 1;
	}
	jx_delete(local_job);

	struct jx *category = jx_eval(jx_lookup(j, "category"), context);
	char *c;
	if (jx_match_string(category, &c)) {
		debug(D_MAKEFLOW_PARSER, "Category %s", c);
		n->category = makeflow_category_lookup_or_create(d, c);
		free(c);
	} else {
		debug(D_MAKEFLOW_PARSER, "category malformed or missing, using default");
		n->category = makeflow_category_lookup_or_create(d, "default");
	}
	jx_delete(category);

	struct jx *resources = jx_eval(jx_lookup(j, "resources"), context);
	if (!resources_from_jx(n->variables, resources)) {
		debug(D_MAKEFLOW_PARSER, "Failure parsing resources");
		return 0;
	}
	jx_delete(resources);

	struct jx *allocation = jx_eval(jx_lookup(j, "allocation"), context);
	char *a;
	if (jx_match_string(allocation, &a)) {
		if (!strcmp(a, "first")) {
			debug(D_MAKEFLOW_PARSER, "first allocation");
			n->resource_request = CATEGORY_ALLOCATION_FIRST;
		} else if (!strcmp(a, "max")) {
			debug(D_MAKEFLOW_PARSER, "max allocation");
			n->resource_request = CATEGORY_ALLOCATION_MAX;
		} else if (!strcmp(a, "error")) {
			debug(D_MAKEFLOW_PARSER, "error allocation");
			n->resource_request = CATEGORY_ALLOCATION_ERROR;
		} else {
			debug(D_MAKEFLOW_PARSER, "Unknown allocation");
			free(a);
			return 0;
		}
		free(a);
	} else {
		debug(D_MAKEFLOW_PARSER, "Allocation malformed or missing");
	}
	jx_delete(allocation);

	struct jx *environment = jx_eval(jx_lookup(j, "environment"), context);
	if (environment) {
		environment_from_jx(d, n, n->variables, environment);
	} else {
		debug(D_MAKEFLOW_PARSER, "environment malformed or missing");
	}
	jx_delete(environment);

	jx_delete(context);
	return 1;
}

static int category_from_jx(struct dag *d, const char *name, struct jx *j) {
	struct category *c = makeflow_category_lookup_or_create(d, name);
	if (!resources_from_jx(c->mf_variables, j)) {
		debug(D_MAKEFLOW_PARSER, "Failure parsing resources");
		return 0;
	}
	struct jx *environment = jx_lookup(j, "environment");
	if (environment && !environment_from_jx(d, NULL, c->mf_variables, environment)) {
		debug(D_MAKEFLOW_PARSER, "Failure parsing environment");
		return 0;
	}
	return 1;
}

// This leaks memory on failure, but it's assumed that if the DAG can't be
// parsed, the program will be exiting soon anyway
struct dag *dag_from_jx(struct jx *j) {
	if (!j) {
		return NULL;
	}

	struct dag *d = dag_create();
	struct jx *dummy_context = jx_object(NULL);
	struct jx *variables = jx_lookup(j, "variables");
	if (variables && !jx_istype(variables, JX_OBJECT)) {
		debug(D_MAKEFLOW_PARSER, "Expected variables to be an object");
		return NULL;
	}

	debug(D_MAKEFLOW_PARSER, "Parsing categories");
	struct jx *categories = jx_lookup(j, "categories");
	if (jx_istype(categories, JX_OBJECT)) {
		struct jx *item;
		void *i = NULL;
		while ((item = jx_iterate_keys(categories, &i))) {
			char *key;
			if (jx_match_string(item, &key)) {
				struct jx *value = jx_lookup(categories, key);
				if (!category_from_jx(d, key, value)) {
					debug(D_MAKEFLOW_PARSER, "Failure parsing category");
					free(key);
					return NULL;
				}
				free(key);
			} else {
				debug(D_MAKEFLOW_PARSER, "Category names must be strings");
				return NULL;
			}
		}
	} else {
		debug(D_MAKEFLOW_PARSER, "categories malformed or missing");
	}

	const char *default_category = jx_lookup_string(j, "default_category");
	if (default_category) {
		debug(D_MAKEFLOW_PARSER, "Default category %s", default_category);
	} else {
		debug(D_MAKEFLOW_PARSER, "default_category malformed or missing, using default");
		default_category = "default";
	}
	d->default_category = makeflow_category_lookup_or_create(d, default_category);

	struct jx *environment = jx_lookup(j, "environment");
	if (environment && !environment_from_jx(d, NULL, d->default_category->mf_variables, environment)) {
		debug(D_MAKEFLOW_PARSER, "Failure parsing top-level category");
		return NULL;
	} else {
		debug(D_MAKEFLOW_PARSER, "Top-level environment malformed or missing");
	}

	struct jx *rules = jx_lookup(j, "rules");
	if (jx_istype(rules, JX_ARRAY)) {
		struct jx *item;
		void *i;
		while ((item = jx_iterate_array(rules, &i))) {
			struct jx *rule_context = jx_merge(variables ? variables : dummy_context, jx_lookup(item, "variables"), NULL);
			if (!rule_from_jx(d, rule_context, item)) {
				debug(D_MAKEFLOW_PARSER, "Failure parsing rule");
				return NULL;
			}
			jx_delete(rule_context);
		}
	}

	dag_close_over_environment(d);
	dag_close_over_categories(d);
	jx_delete(dummy_context);
	return d;
}

