<?php

/**

 * @file

 * Assistant thread and session helpers.

 */

function ai_assistants_chat_pending_key($chat_instance_id) {
  return 'ai_assistants_chat_pending:' . (string) $chat_instance_id;
}

/**
 * Loads pending approval state for an assistant.
 */
function ai_assistants_chat_get_pending($chat_instance_id) {
  $key = ai_assistants_chat_pending_key($chat_instance_id);
  return !empty($_SESSION[$key]) && is_array($_SESSION[$key]) ? $_SESSION[$key] : [];
}

/**
 * Stores pending approval state for an assistant.
 */
function ai_assistants_chat_set_pending($chat_instance_id, array $pending) {
  $_SESSION[ai_assistants_chat_pending_key($chat_instance_id)] = $pending;
}

/**
 * Clears pending approval state for an assistant.
 */
function ai_assistants_chat_clear_pending($chat_instance_id) {
  unset($_SESSION[ai_assistants_chat_pending_key($chat_instance_id)]);
}

/**
 * Persists the routing plan that produced an approval pause.
 *
 * Keyed by thread_id so resume can replay the same plan instead of
 * re-running the (non-deterministic) LLM router.
 */
function ai_assistants_set_pending_plan($thread_id, array $payload) {
  $thread_id = (string) $thread_id;
  if ($thread_id === '') {
    return;
  }
  $plans = state_get('ai_assistants_pending_plans', []);
  if (!is_array($plans)) {
    $plans = [];
  }
  // Garbage collect expired entries before writing.
  foreach ($plans as $key => $entry) {
    if (!empty($entry['expires']) && (int) $entry['expires'] < REQUEST_TIME) {
      unset($plans[$key]);
    }
  }
  $payload['expires'] = REQUEST_TIME + 86400;
  $plans[$thread_id] = $payload;
  state_set('ai_assistants_pending_plans', $plans);
}

/**
 * Loads the routing plan stored when an approval pause occurred.
 */
function ai_assistants_get_pending_plan($thread_id) {
  $thread_id = (string) $thread_id;
  if ($thread_id === '') {
    return [];
  }
  $plans = state_get('ai_assistants_pending_plans', []);
  if (!is_array($plans) || empty($plans[$thread_id])) {
    return [];
  }
  $entry = $plans[$thread_id];
  if (!empty($entry['expires']) && (int) $entry['expires'] < REQUEST_TIME) {
    return [];
  }
  return is_array($entry) ? $entry : [];
}

/**
 * Clears any stored routing plan for a thread.
 */
function ai_assistants_clear_pending_plan($thread_id) {
  $thread_id = (string) $thread_id;
  if ($thread_id === '') {
    return;
  }
  $plans = state_get('ai_assistants_pending_plans', []);
  if (!is_array($plans) || !array_key_exists($thread_id, $plans)) {
    return;
  }
  unset($plans[$thread_id]);
  state_set('ai_assistants_pending_plans', $plans);
}

/**
 * Returns default block config from site config.
 */
function ai_assistants_block_default_config() {
  $config = config_get('ai_assistants.settings', 'block_config');
  return is_array($config) ? $config : [];
}

/**
 * Normalizes per-block assistant settings.
 */
function ai_assistants_block_normalize_settings(array $settings = []) {
  $defaults = ai_assistants_block_default_config() + [
    'custom_title' => '',
    'assistant_id' => '',
    'instance_id' => '',
  ];
  $instance = [];
  if (!empty($settings['ai_assistants_block']) && is_array($settings['ai_assistants_block'])) {
    $instance = $settings['ai_assistants_block'];
  }
  elseif (!empty($settings['block_settings']['ai_assistants_block']) && is_array($settings['block_settings']['ai_assistants_block'])) {
    $instance = $settings['block_settings']['ai_assistants_block'];
  }

  return [
    'custom_title' => isset($instance['custom_title']) ? trim((string) $instance['custom_title']) : (string) $defaults['custom_title'],
    'assistant_id' => isset($instance['assistant_id']) ? trim((string) $instance['assistant_id']) : (string) $defaults['assistant_id'],
    'instance_id' => isset($instance['instance_id']) ? trim((string) $instance['instance_id']) : (string) $defaults['instance_id'],
  ];
}

/**
 * Returns stable per-block chat instance id.
 */
function ai_assistants_block_instance_id(array $block_config) {
  if (!empty($block_config['instance_id'])) {
    return (string) $block_config['instance_id'];
  }
  return 'legacy-' . substr(hash('sha256', serialize($block_config)), 0, 16);
}

/**
 * Returns a session key for one chat block instance.
 */
function ai_assistants_chat_session_key($suffix, $chat_instance_id) {
  return 'ai_assistants_chat_' . $suffix . ':' . (string) $chat_instance_id;
}

/**
 * Returns thread data.
 */
function ai_assistants_get_thread($thread_id) {
  if ($thread_id === '') {
    return [];
  }

  $cached_threads = ai_assistants_thread_cache();
  if (!empty($cached_threads[$thread_id]) && is_array($cached_threads[$thread_id])) {
    return ai_assistants_normalize_thread($cached_threads[$thread_id]);
  }
  if (!empty($_SESSION['ai_assistants_threads'][$thread_id]) && is_array($_SESSION['ai_assistants_threads'][$thread_id])) {
    return ai_assistants_normalize_thread($_SESSION['ai_assistants_threads'][$thread_id]);
  }

  if (substr($thread_id, 0, 8) === 'session:') {
    return [];
  }

  if (substr($thread_id, 0, 8) === 'storage:') {
    $stored = cache_get($thread_id, 'cache');
    return !empty($stored->data) && is_array($stored->data) ? $stored->data : [];
  }

  $row = db_select('ai_assistants_thread', 't')
    ->fields('t')
    ->condition('thread_id', $thread_id)
    ->execute()
    ->fetchAssoc();

  if (empty($row)) {
    return [];
  }

  // A stored thread owned by an authenticated user is only readable by that
  // user. Callers must not be able to read someone else's transcript by
  // supplying a foreign thread id.
  if (!ai_assistants_thread_owner_matches($row)) {
    return [];
  }

  // Enforce the assistant's configured thread TTL (0 = never expire).
  $ttl = ai_assistants_thread_ttl(!empty($row['assistant_id']) ? (string) $row['assistant_id'] : '');
  $updated = isset($row['updated']) ? (int) $row['updated'] : 0;
  if ($ttl > 0 && ($updated + $ttl) < REQUEST_TIME) {
    return [];
  }

  $row['messages'] = !empty($row['messages']) ? json_decode($row['messages'], TRUE) : [];
  if (!is_array($row['messages'])) {
    $row['messages'] = [];
  }
  return $row;
}

/**
 * Returns the configured thread TTL in seconds for one assistant.
 */
function ai_assistants_thread_ttl($assistant_id) {
  if ($assistant_id === '') {
    return 0;
  }
  $assistant = ai_assistants_load($assistant_id);
  return !empty($assistant['thread_ttl']) ? max(0, (int) $assistant['thread_ttl']) : 0;
}

/**
 * Clears thread data.
 */
function ai_assistants_clear_thread($thread_id) {
  if ($thread_id === '') {
    return;
  }

  $cached_threads = ai_assistants_thread_cache();
  unset($cached_threads[$thread_id]);
  ai_assistants_thread_cache($cached_threads);
  unset($_SESSION['ai_assistants_threads'][$thread_id]);

  if (substr($thread_id, 0, 8) === 'session:') {
    return;
  }

  $existing = db_query("SELECT uid FROM {ai_assistants_thread} WHERE thread_id = :tid", [':tid' => $thread_id])->fetchAssoc();
  if (!empty($existing) && !ai_assistants_thread_owner_matches($existing)) {
    return;
  }

  db_delete('ai_assistants_thread')
    ->condition('thread_id', $thread_id)
    ->execute();
}

/**
 * Stores thread data.
 */
function ai_assistants_save_thread($thread_id, $assistant_id, array $messages, array $assistant, array $options = []) {
  $storage = !empty($assistant['thread_storage']) ? (string) $assistant['thread_storage'] : 'session';
  $record = [
    'thread_id' => $thread_id,
    'assistant_id' => $assistant_id,
    'uid' => isset($GLOBALS['user']->uid) ? (int) $GLOBALS['user']->uid : 0,
    'messages' => $messages,
    'created' => REQUEST_TIME,
    'updated' => REQUEST_TIME,
  ];

  if ($storage === 'none') {
    return;
  }

  if ($storage === 'session') {
    $messages = array_values($messages);
    $record['messages'] = $messages;
    $cached_threads = ai_assistants_thread_cache();
    $cached_threads[$thread_id] = $record;
    ai_assistants_thread_cache($cached_threads);
    $_SESSION['ai_assistants_threads'][$thread_id] = $record;
    return;
  }

  // Never overwrite a thread owned by a different authenticated user.
  $existing = db_query("SELECT uid, assistant_id FROM {ai_assistants_thread} WHERE thread_id = :tid", [':tid' => $thread_id])->fetchAssoc();
  if (!empty($existing) && !ai_assistants_thread_owner_matches($existing)) {
    watchdog('ai_assistants', 'Refused to write thread @tid owned by uid @owner from uid @actor.', [
      '@tid' => $thread_id,
      '@owner' => (int) $existing['uid'],
      '@actor' => $record['uid'],
    ], WATCHDOG_WARNING);
    return;
  }
  if (!empty($existing) && (string) $existing['assistant_id'] !== (string) $assistant_id) {
    watchdog('ai_assistants', 'Refused to reassign thread @tid from assistant @existing to @requested.', [
      '@tid' => $thread_id,
      '@existing' => (string) $existing['assistant_id'],
      '@requested' => (string) $assistant_id,
    ], WATCHDOG_WARNING);
    return;
  }

  db_merge('ai_assistants_thread')
    ->key(['thread_id' => $thread_id])
    ->fields([
      'assistant_id' => $assistant_id,
      'uid' => $record['uid'],
      'messages' => json_encode($messages),
      'created' => REQUEST_TIME,
      'updated' => REQUEST_TIME,
    ])
    ->execute();
}

/**
 * Returns TRUE when the current user may access a stored thread row.
 *
 * Threads owned by an authenticated user (uid > 0) are private to that user.
 * Anonymous-owned threads (uid 0) are gated only by the unguessable random
 * thread id, since anonymous users have no other stable identity.
 */
function ai_assistants_thread_owner_matches(array $row) {
  $owner = isset($row['uid']) ? (int) $row['uid'] : 0;
  if ($owner === 0) {
    return TRUE;
  }
  $current = isset($GLOBALS['user']->uid) ? (int) $GLOBALS['user']->uid : 0;
  return $owner === $current;
}

/**
 * Returns or replaces in-request assistant thread cache.
 */
function ai_assistants_thread_cache(?array $threads = NULL) {
  $cache = &backdrop_static(__FUNCTION__, []);
  if (isset($threads)) {
    $cache = $threads;
  }
  return $cache;
}

/**
 * Normalizes a thread record before returning it.
 */
function ai_assistants_normalize_thread(array $thread) {
  if (empty($thread['messages']) || !is_array($thread['messages'])) {
    $thread['messages'] = [];
  }
  else {
    $thread['messages'] = array_values($thread['messages']);
  }
  return $thread;
}

/**
 * Composes a thread transcript for future prompt expansion.
 */
function ai_assistants_compose_message(array $messages, $message) {
  $parts = [];
  foreach ($messages as $entry) {
    if (empty($entry['role'])) {
      continue;
    }
    $parts[] = strtoupper((string) $entry['role']) . ': ' . (isset($entry['content']) ? (string) $entry['content'] : '');
  }
  $parts[] = 'CURRENT_USER_MESSAGE: ' . (string) $message;
  return implode("\n\n", $parts);
}

/**
 * Creates a new thread id.
 */
function ai_assistants_create_thread_id($storage = 'session') {
  if ($storage === 'database') {
    return backdrop_random_key(16);
  }
  return 'session:' . backdrop_random_key(16);
}

/**
 * Checks if the current user can use this assistant.
 */
function ai_assistants_user_allowed(array $assistant) {
  if (empty($assistant['allowed_roles']) || !is_array($assistant['allowed_roles'])) {
    return TRUE;
  }

  global $user;
  $user_role_map = !empty($user->roles) && is_array($user->roles) ? $user->roles : [];
  $allowed_roles = ai_assistants_normalize_role_aliases($assistant['allowed_roles']);
  $current_roles = ai_assistants_normalize_role_aliases(array_merge(array_keys($user_role_map), array_values($user_role_map)));

  return (bool) array_intersect($current_roles, $allowed_roles);
}

/**
 * Normalize common Backdrop role aliases.
 */
function ai_assistants_normalize_role_aliases(array $roles) {
  $aliases = [
    'anonymous user' => 'anonymous',
    'authenticated user' => 'authenticated',
  ];

  $normalized = [];
  foreach ($roles as $role) {
    $role = trim(strtolower((string) $role));
    if ($role === '') {
      continue;
    }
    $normalized[] = isset($aliases[$role]) ? $aliases[$role] : $role;
  }

  return array_values(array_unique($normalized));
}

/**
 * Substitute well-known tokens in an assistant prompt fragment.
 *
 * Mirrors the placeholders used by Drupal's ai_assistant_api system prompt
 * (`[user_name]`, `[user_roles]`, `[page_title]`, `[page_path]`, `[site_name]`)
 * so the same content patterns work across both ecosystems. Unknown tokens
 * are left intact.
 */
function ai_assistants_substitute_prompt_tokens($prompt) {
  if (!is_string($prompt) || $prompt === '' || strpos($prompt, '[') === FALSE) {
    return (string) $prompt;
  }

  global $user;
  $username = (!empty($user) && !empty($user->name)) ? (string) $user->name : t('Anonymous');
  $roles = [];
  if (!empty($user) && !empty($user->roles) && is_array($user->roles)) {
    $roles = array_values($user->roles);
  }

  $page_title = function_exists('backdrop_get_title') ? backdrop_get_title() : '';
  if (!is_string($page_title)) {
    $page_title = '';
  }

  $page_path = function_exists('current_path') ? current_path() : '';
  if ($page_path !== '') {
    $page_path = '/' . ltrim($page_path, '/');
  }

  $site_name = function_exists('config_get') ? (string) config_get('system.core', 'site_name') : '';
  if ($site_name === '') {
    $site_name = 'this site';
  }

  $replacements = [
    '[user_name]' => $username,
    '[user_roles]' => empty($roles) ? t('none') : implode(', ', $roles),
    '[page_title]' => $page_title !== '' ? $page_title : t('(untitled)'),
    '[page_path]' => $page_path !== '' ? $page_path : '/',
    '[site_name]' => $site_name,
  ];

  return strtr($prompt, $replacements);
}
