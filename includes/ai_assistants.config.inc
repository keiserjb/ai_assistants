<?php

/**
 * @file
 * Assistant config and registry helpers.
 */

define('AI_ASSISTANTS_CONFIG_PREFIX', 'ai_assistants.assistant.');

/**
 * Return the config object name for one assistant.
 */
function ai_assistants_config_name($assistant_id) {
  return AI_ASSISTANTS_CONFIG_PREFIX . $assistant_id;
}

/**
 * Return all per-assistant config object names.
 */
function ai_assistants_config_names() {
  return config_get_names_with_prefix(AI_ASSISTANTS_CONFIG_PREFIX);
}

/**
 * Return assistant definitions from the legacy monolithic settings blob.
 */
function ai_assistants_get_legacy_assistants() {
  $assistants = config_get('ai_assistants.settings', 'assistants');
  return is_array($assistants) ? $assistants : [];
}

/**
 * Save a single assistant into its own config object.
 */
function ai_assistants_save_assistant($assistant_id, array $assistant) {
  $config = config(ai_assistants_config_name($assistant_id));
  $config->setData($assistant);
  $config->save();
}

/**
 * Return all assistant definitions, merging per-object configs with legacy blob.
 */
function ai_assistants_get_assistants() {
  $assistants = [];
  $config_names = ai_assistants_config_names();

  if (!empty($config_names)) {
    foreach (config_load_multiple($config_names) as $config_name => $data) {
      if (!is_array($data)) {
        continue;
      }
      $assistant_id = substr($config_name, strlen(AI_ASSISTANTS_CONFIG_PREFIX));
      if ($assistant_id === '') {
        continue;
      }
      $assistants[$assistant_id] = $data;
    }
  }

  foreach (ai_assistants_get_legacy_assistants() as $assistant_id => $assistant) {
    if (!isset($assistants[$assistant_id]) && is_array($assistant)) {
      $assistants[$assistant_id] = $assistant;
    }
  }

  ksort($assistants);
  return $assistants;
}

/**
 * Returns a UUID string.
 */
function ai_assistants_generate_uuid() {
  if (function_exists('backdrop_generate_uuid')) {
    return backdrop_generate_uuid();
  }
  $uuid = new Uuid();
  return $uuid->generate();
}

/**
 * Machine-name lookup.
 */
function ai_assistants_admin_exists($assistant_id) {
  $assistants = ai_assistants_get_assistants();
  return isset($assistants[$assistant_id]);
}

/**
 * Loads one assistant.
 */
function ai_assistants_load($assistant_id) {
  $assistant = config_get(ai_assistants_config_name($assistant_id));
  if (is_array($assistant) && !empty($assistant)) {
    return $assistant;
  }

  $assistants = ai_assistants_get_legacy_assistants();
  return isset($assistants[$assistant_id]) && is_array($assistants[$assistant_id]) ? $assistants[$assistant_id] : [];
}

/**
 * Saves assistants to per-assistant config objects.
 */
function ai_assistants_save_assistants(array $assistants) {
  $existing_ids = array_keys(ai_assistants_get_assistants());

  foreach ($assistants as $assistant_id => $assistant) {
    ai_assistants_save_assistant($assistant_id, $assistant);
  }

  foreach (array_diff($existing_ids, array_keys($assistants)) as $assistant_id) {
    config(ai_assistants_config_name($assistant_id))->delete();
  }

  config_clear('ai_assistants.settings', 'assistants');
}

/**
 * Deletes one assistant.
 */
function ai_assistants_delete($assistant_id) {
  if (empty(ai_assistants_load($assistant_id))) {
    return FALSE;
  }

  $config = config(ai_assistants_config_name($assistant_id));
  if (!$config->isNew()) {
    $config->delete();
  }

  $legacy = ai_assistants_get_legacy_assistants();
  if (isset($legacy[$assistant_id])) {
    unset($legacy[$assistant_id]);
    $settings = config('ai_assistants.settings');
    if (!empty($legacy)) {
      $settings->set('assistants', $legacy);
    }
    else {
      $settings->clear('assistants');
    }
    $settings->save();
  }

  return TRUE;
}

/**
 * Returns assistant options for selects.
 */
function ai_assistants_options() {
  $options = [];
  foreach (ai_assistants_get_assistants() as $assistant_id => $assistant) {
    $label = !empty($assistant['label']) ? $assistant['label'] : $assistant_id;
    $options[$assistant_id] = $label . ' (' . $assistant_id . ')';
  }
  return $options;
}

/**
 * Returns available RAG index options for assistant config.
 */
function ai_assistants_rag_index_options() {
  if (!module_exists('ai_search') || !function_exists('ai_search_list_rag_indexes')) {
    return [];
  }
  $available_indexes = ai_search_list_rag_indexes();
  if (empty($available_indexes['indexes']) || !is_array($available_indexes['indexes'])) {
    return [];
  }

  $options = [];
  foreach ($available_indexes['indexes'] as $index_info) {
    if (empty($index_info['index_id'])) {
      continue;
    }
    $label = !empty($index_info['label']) ? $index_info['label'] : $index_info['index_id'];
    $backend = !empty($index_info['backend']) ? ' (' . $index_info['backend'] . ')' : '';
    $options[$index_info['index_id']] = $label . $backend;
  }

  return $options;
}

/**
 * Returns route mode options.
 */
function ai_assistants_route_mode_options() {
  return [
    'direct' => t('Direct answer'),
    'single_agent' => t('Single agent'),
    'multi_agent' => t('Multiple agents'),
    'router' => t('Auto router'),
  ];
}

/**
 * Returns configured agent ids for an assistant.
 */
function ai_assistants_get_agent_ids(array $assistant) {
  $agent_ids = [];
  if (!empty($assistant['agent_ids']) && is_array($assistant['agent_ids'])) {
    $agent_ids = array_values(array_filter($assistant['agent_ids']));
  }
  if (!empty($assistant['agent_id'])) {
    array_unshift($agent_ids, (string) $assistant['agent_id']);
  }
  return array_values(array_unique(array_filter($agent_ids)));
}

/**
 * Resolve assistant route mode.
 */
function ai_assistants_get_route_mode(array $assistant) {
  $route_mode = !empty($assistant['route_mode']) ? (string) $assistant['route_mode'] : '';
  return isset(ai_assistants_route_mode_options()[$route_mode]) ? $route_mode : '';
}
