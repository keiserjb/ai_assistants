<?php

/**

 * @file

 * Assistant runtime execution helpers.

 */

function ai_assistants_default_model(array $assistant = []) {
  return !empty($assistant['model']) ? (string) $assistant['model'] : '';
}

/**
 * Build assistant-level prompt messages for direct chat mode.
 *
 * Note: pre_action_prompt is intentionally NOT included here. That field is
 * the router prompt template used by router/multi_agent modes and contains
 * routing placeholders ([examples], [usage_instructions]) that are not
 * meaningful in direct chat. Put direct-chat instructions in system_prompt.
 */
function ai_assistants_build_chat_messages(array $assistant, array $thread_messages, $message) {
  $messages = [];

  if (!empty($assistant['system_prompt'])) {
    $messages[] = [
      'role' => 'system',
      'content' => ai_assistants_substitute_prompt_tokens((string) $assistant['system_prompt']),
    ];
  }

  foreach ($thread_messages as $entry) {
    if (empty($entry['role']) || !isset($entry['content'])) {
      continue;
    }
    $messages[] = [
      'role' => (string) $entry['role'],
      'content' => (string) $entry['content'],
    ];
  }

  if (!empty($assistant['secured_system_prompt'])) {
    $messages[] = [
      'role' => 'system',
      'content' => ai_assistants_substitute_prompt_tokens((string) $assistant['secured_system_prompt']),
    ];
  }

  $messages[] = [
    'role' => 'user',
    'content' => (string) $message,
  ];

  return $messages;
}

/**
 * Build assistant message payload for agent mode.
 *
 * Important: the assistant's system_prompt is intentionally NOT injected here.
 * The system_prompt governs the assistant's user-facing voice (Preview/Recap
 * pattern, audience, formatting). Each backing agent has its own system_prompt
 * that defines its domain — leaking the assistant-level instructions into the
 * agent runtime causes the agent to try to follow Preview/Recap pattern itself
 * (e.g. asking the user about phase 2 it cannot perform).
 *
 * Only secured_system_prompt is injected, because that field is for hard
 * security guardrails the assistant wants enforced inside every agent run.
 */
function ai_assistants_build_agent_message(array $assistant, array $thread_messages, $message) {
  $parts = [];
  $has_prompts = FALSE;

  if (!empty($assistant['secured_system_prompt'])) {
    $parts[] = "ASSISTANT_SECURED_PROMPT:\n" . ai_assistants_substitute_prompt_tokens((string) $assistant['secured_system_prompt']);
    $has_prompts = TRUE;
  }

  if (empty($thread_messages) && !$has_prompts) {
    return (string) $message;
  }

  if (!empty($thread_messages)) {
    $parts[] = ai_assistants_compose_message($thread_messages, $message);
  }
  else {
    $parts[] = 'CURRENT_USER_MESSAGE: ' . (string) $message;
  }

  return implode("\n\n", $parts);
}

/**
 * Find active assistant errors for a user.
 */
function ai_assistants_access_error(array $assistant) {
  if (!ai_assistants_user_allowed($assistant)) {
    return t('This assistant is restricted to its allowed roles. If you need help, ask an administrator or switch to an assistant you can use.');
  }
  return '';
}

/**
 * Run one backing agent and normalize result.
 */
function ai_assistants_run_backing_agent($assistant_id, array $assistant, $agent_id, $message, array $options = []) {
  $assistant_model = ai_assistants_default_model($assistant);
  if ($assistant_model !== '' && empty($options['model'])) {
    $options['model'] = $assistant_model;
  }
  if (!empty($assistant['rag_index_ids']) && is_array($assistant['rag_index_ids'])) {
    $rag_index_ids = array_values(array_filter(array_map('strval', $assistant['rag_index_ids'])));
    if (!empty($rag_index_ids)) {
      // Assistant-level RAG settings override the backing agent's defaults so
      // one assistant can be pinned to a specific index set.
      if (empty($options['tool_settings_overrides']) || !is_array($options['tool_settings_overrides'])) {
        $options['tool_settings_overrides'] = [];
      }
      $options['tool_settings_overrides']['ai_search_rag_search'] = [
        'forced_params' => [
          'index_ids' => $rag_index_ids,
        ],
      ];
    }
  }
  $result = ai_agents_run($agent_id, (string) $message, $options);
  if (is_string($result)) {
    $result = [
      'answer' => '',
      'loops' => 0,
      'tool_calls' => [],
      'error' => 'Unexpected async job response from agent runner.',
    ];
  }

  $agent = ai_agents_load($agent_id);
  $agent_label = !empty($agent['label']) ? $agent['label'] : $agent_id;

  return [
    'assistant_id' => $assistant_id,
    'agent_id' => $agent_id,
    'agent_label' => $agent_label,
    'answer' => isset($result['answer']) ? (string) $result['answer'] : '',
    'structured' => isset($result['structured']) ? $result['structured'] : NULL,
    'status' => !empty($result['status']) ? (string) $result['status'] : (empty($result['error']) ? 'ok' : 'error'),
    'token' => isset($result['token']) ? (string) $result['token'] : NULL,
    'pending' => isset($result['pending_tool']) ? $result['pending_tool'] : NULL,
    'loops' => isset($result['loops']) ? (int) $result['loops'] : 0,
    'tool_calls' => isset($result['tool_calls']) && is_array($result['tool_calls']) ? $result['tool_calls'] : [],
    'error' => isset($result['error']) ? $result['error'] : NULL,
  ];
}

/**
 * Format multi-agent results into a single answer string.
 *
 * When at least one agent actually performed work (has tool_calls or produced
 * a declared output), agents that did not perform work are suppressed —
 * those are typically off-topic agents that fell through to "I cannot do this
 * directly" prose because their tools did not cover the request.
 */
function ai_assistants_format_multi_agent_answer(array $results) {
  $acting_indices = [];
  foreach ($results as $index => $result) {
    if (ai_assistants_result_did_actioning_work($result)) {
      $acting_indices[$index] = TRUE;
      continue;
    }
    if (!empty($result['agent_id'])) {
      $info = ai_assistants_orchestration_info((string) $result['agent_id']);
      foreach ($info['produces'] as $callable) {
        $value = call_user_func($callable, $result);
        if ($value !== NULL && $value !== '' && $value !== [] && $value !== FALSE) {
          $acting_indices[$index] = TRUE;
          break;
        }
      }
    }
  }
  $any_acted = !empty($acting_indices);

  $parts = [];
  foreach ($results as $index => $result) {
    if (empty($result['answer']) && empty($result['error'])) {
      continue;
    }
    if ($any_acted && empty($acting_indices[$index]) && empty($result['error'])) {
      // Suppress agents that did not act when at least one peer did. Their
      // prose is almost always off-topic ("I cannot do this directly...").
      continue;
    }
    $title = !empty($result['agent_label']) ? $result['agent_label'] : $result['agent_id'];
    $body = !empty($result['answer']) ? $result['answer'] : $result['error'];
    $parts[] = $title . "\n" . $body;
  }
  // If filtering left only one acting agent, drop the agent label header for
  // a cleaner single-voice response.
  if (count($parts) === 1 && $any_acted) {
    $first = $parts[0];
    $newline_position = strpos($first, "\n");
    if ($newline_position !== FALSE) {
      return ltrim(substr($first, $newline_position + 1));
    }
  }
  return implode("\n\n---\n\n", $parts);
}

/**
 * Returns TRUE if a result includes a tool call that performs side-effects.
 *
 * Heuristic based on common verb prefixes for actioning tools (create_,
 * modify_, update_, add_, delete_, set_, save_, remove_, write_, generate_,
 * import_, export_, run_, execute_). Read-only discovery tools (get_*,
 * list_*, find_*, search_*) are excluded so an agent that merely inspected
 * the site is not counted as "acting."
 */
function ai_assistants_result_did_actioning_work(array $result) {
  if (empty($result['tool_calls']) || !is_array($result['tool_calls'])) {
    return FALSE;
  }
  $action_prefixes = [
    'create_', 'modify_', 'update_', 'add_', 'delete_', 'remove_',
    'set_', 'save_', 'write_', 'generate_', 'import_', 'export_',
    'run_', 'execute_', 'enable_', 'disable_', 'install_', 'uninstall_',
  ];
  foreach ($result['tool_calls'] as $entry) {
    $inner = !empty($entry['tool_calls']) && is_array($entry['tool_calls']) ? $entry['tool_calls'] : [];
    foreach ($inner as $tool_call) {
      $name = !empty($tool_call['name']) ? (string) $tool_call['name'] : '';
      if ($name === '') {
        continue;
      }
      foreach ($action_prefixes as $prefix) {
        if (strncmp($name, $prefix, strlen($prefix)) === 0) {
          return TRUE;
        }
      }
    }
  }
  return FALSE;
}

/**
 * Execute an explicit agent plan.
 *
 * Generic orchestrator: agents run in the order supplied by the plan, with
 * declared produces/consumes callbacks (see ai_assistants.orchestrator.inc)
 * piping outputs from earlier agents into the runtime messages of later ones.
 * No agent_id is recognized by name in this function.
 */
function ai_assistants_execute_agent_plan($assistant_id, array $assistant, array $thread_messages, $message, array $plan, array $agent_options = []) {
  $response = [
    'answer' => '',
    'structured' => NULL,
    'status' => 'error',
    'token' => NULL,
    'pending' => NULL,
    'loops' => 0,
    'tool_calls' => [],
    'error' => NULL,
  ];

  $agent_ids = !empty($plan['agent_ids']) && is_array($plan['agent_ids']) ? $plan['agent_ids'] : [];
  if (empty($agent_ids)) {
    $response['error'] = t('No backing agent matches the requested action.');
    return $response;
  }

  $strategy = !empty($plan['strategy']) ? (string) $plan['strategy'] : (count($agent_ids) > 1 ? 'ask_many' : 'choose_one');
  if ($strategy === 'workflow') {
    return ai_assistants_execute_workflow_plan($assistant_id, $assistant, $message, $plan, $agent_options);
  }

  $base_runtime_message = ai_assistants_build_agent_message($assistant, $thread_messages, $message);
  $isolated_runtime_message = ai_assistants_build_agent_message($assistant, [], $message);

  if (count($agent_ids) === 1) {
    $agent_id = (string) reset($agent_ids);
    $result = ai_assistants_run_backing_agent($assistant_id, $assistant, $agent_id, $base_runtime_message, $agent_options);
    $response['answer'] = $result['answer'];
    $response['structured'] = $result['structured'];
    $response['status'] = $result['status'];
    $response['token'] = $result['token'];
    $response['pending'] = $result['pending'];
    $response['loops'] = $result['loops'];
    $response['tool_calls'] = $result['tool_calls'];
    $response['error'] = $result['error'];
    $response['agent_id'] = $agent_id;
    $response['agent_label'] = $result['agent_label'];
    $response['strategy'] = 'choose_one';
    return $response;
  }

  $results = [];
  foreach ($agent_ids as $agent_id) {
    $agent_id = (string) $agent_id;
    $info = ai_assistants_orchestration_info($agent_id);
    // Use the isolated runtime message for any consumer agent so prior
    // transcript context cannot leak past the explicit produced inputs.
    $runtime_message = !empty($info['consumes']) ? $isolated_runtime_message : $base_runtime_message;
    $outputs = ai_assistants_orchestration_collect_outputs($results);
    $runtime_message = ai_assistants_orchestration_inject($agent_id, $runtime_message, $outputs);

    $result = ai_assistants_run_backing_agent($assistant_id, $assistant, $agent_id, $runtime_message, $agent_options);
    $results[] = $result;
    if ($result['status'] === 'needs_approval') {
      $response['status'] = 'needs_approval';
      $response['token'] = $result['token'];
      $response['pending'] = $result['pending'];
      $response['error'] = NULL;
      $response['tool_calls'] = $result['tool_calls'];
      $response['loops'] = $result['loops'];
      $response['assistant_id'] = $assistant_id;
      $response['agent_id'] = $agent_id;
      $response['pending_agent_id'] = $agent_id;
      $response['strategy'] = $strategy;
      return $response;
    }
  }

  if (empty($response['answer'])) {
    $response['answer'] = ai_assistants_format_multi_agent_answer($results);
  }
  $response['structured'] = NULL;
  if ($response['status'] !== 'error') {
    $response['status'] = !empty($response['answer']) ? 'ok' : 'error';
  }
  foreach ($results as $result) {
    foreach ($result['tool_calls'] as $tool_call) {
      $tool_call['agent_id'] = $result['agent_id'];
      $response['tool_calls'][] = $tool_call;
    }
  }
  $response['loops'] = ai_assistants_sum_result_loops($results);
  if ($response['status'] === 'error' && empty($response['error'])) {
    $response['error'] = t('No backing agents returned a response.');
  }
  $response['strategy'] = $strategy;

  return $response;
}

/**
 * Execute a structured workflow plan.
 */
function ai_assistants_execute_workflow_plan($assistant_id, array $assistant, $message, array $plan, array $agent_options = [], array $seed_results = []) {
  $response = [
    'answer' => '',
    'structured' => NULL,
    'status' => 'error',
    'token' => NULL,
    'pending' => NULL,
    'loops' => 0,
    'tool_calls' => [],
    'error' => NULL,
    'strategy' => 'workflow',
    'structured' => [
      'plan' => !empty($plan['actions']) && is_array($plan['actions']) ? array_values($plan['actions']) : [],
      'results' => [],
    ],
  ];

  $workflow = !empty($plan['workflow']) && is_array($plan['workflow']) ? array_values($plan['workflow']) : [];
  if (empty($workflow)) {
    $response['error'] = t('No workflow steps were available for this request.');
    return $response;
  }

  $results = $seed_results;

  for ($i = 0; $i < count($workflow); $i++) {
    $node = $workflow[$i];
    $agent_id = !empty($node['agent_id']) ? (string) $node['agent_id'] : '';
    if ($agent_id === '') {
      continue;
    }

    $runtime_message = ai_assistants_build_workflow_node_message($assistant, $message, $agent_id, $results);
    $result = ai_assistants_run_backing_agent($assistant_id, $assistant, $agent_id, $runtime_message, $agent_options);
    $results[] = $result;

    if ($result['status'] === 'needs_approval') {
      $response['status'] = 'needs_approval';
      $response['token'] = $result['token'];
      $response['pending'] = $result['pending'];
      $response['loops'] = ai_assistants_sum_result_loops($results);
      $response['tool_calls'] = ai_assistants_collect_result_tool_calls($results);
      $response['agent_id'] = $agent_id;
      $response['pending_agent_id'] = $agent_id;
      $response['structured']['results'] = ai_assistants_workflow_results_payload($results);
      $response['answer'] = ai_assistants_synthesize_workflow_response($assistant, $message, $plan, $results);
      return $response;
    }

    if (!empty($result['error'])) {
      $response['status'] = 'error';
      $response['error'] = $result['error'];
      $response['loops'] = ai_assistants_sum_result_loops($results);
      $response['tool_calls'] = ai_assistants_collect_result_tool_calls($results);
      $response['structured']['results'] = ai_assistants_workflow_results_payload($results);
      $response['answer'] = ai_assistants_synthesize_workflow_response($assistant, $message, $plan, $results);
      return $response;
    }

    if (!ai_assistants_workflow_node_satisfied($result)) {
      $response['status'] = 'error';
      $response['error'] = t('Workflow step @agent did not complete with a verifiable result.', ['@agent' => $agent_id]);
      $response['loops'] = ai_assistants_sum_result_loops($results);
      $response['tool_calls'] = ai_assistants_collect_result_tool_calls($results);
      $response['structured']['results'] = ai_assistants_workflow_results_payload($results);
      $response['answer'] = ai_assistants_synthesize_workflow_response($assistant, $message, $plan, $results);
      return $response;
    }
  }

  $response['structured']['results'] = ai_assistants_workflow_results_payload($results);
  $response['answer'] = ai_assistants_synthesize_workflow_response($assistant, $message, $plan, $results);
  $response['status'] = !empty($response['answer']) ? 'ok' : 'error';
  $response['loops'] = ai_assistants_sum_result_loops($results);
  $response['tool_calls'] = ai_assistants_collect_result_tool_calls($results);
  if ($response['status'] === 'error' && empty($response['error'])) {
    $response['error'] = t('No workflow agents returned a response.');
  }

  return $response;
}

/**
 * Build the runtime message for one workflow node.
 *
 * Each workflow node is just an agent. Upstream results' produces callbacks
 * are collected and piped into this agent via its consumes callbacks. No
 * workflow step types are recognized here.
 */
function ai_assistants_build_workflow_node_message(array $assistant, $message, $agent_id, array $prior_results = []) {
  $runtime_message = ai_assistants_build_agent_message($assistant, [], $message);
  $outputs = ai_assistants_orchestration_collect_outputs($prior_results);
  return ai_assistants_orchestration_inject((string) $agent_id, $runtime_message, $outputs);
}

/**
 * Build a normalized workflow result payload for synthesis and debugging.
 */
function ai_assistants_workflow_results_payload(array $results) {
  $payload = [];
  foreach ($results as $result) {
    $payload[] = [
      'agent_id' => !empty($result['agent_id']) ? (string) $result['agent_id'] : '',
      'agent_label' => !empty($result['agent_label']) ? (string) $result['agent_label'] : '',
      'status' => !empty($result['status']) ? (string) $result['status'] : 'ok',
      'answer' => isset($result['answer']) ? (string) $result['answer'] : '',
      'error' => isset($result['error']) ? $result['error'] : NULL,
      'tool_calls' => !empty($result['tool_calls']) && is_array($result['tool_calls']) ? $result['tool_calls'] : [],
    ];
  }
  return [
    'outputs' => ai_assistants_orchestration_collect_outputs($results),
    'steps' => $payload,
  ];
}

/**
 * Synthesize a final assistant answer over workflow execution output.
 */
function ai_assistants_synthesize_workflow_response(array $assistant, $message, array $plan, array $results) {
  $fallback = ai_assistants_format_multi_agent_answer($results);
  if (!function_exists('ai_chat')) {
    return $fallback;
  }

  $model = ai_assistants_default_model($assistant);
  if ($model === '') {
    return $fallback;
  }

  $synthesis_messages = [
    [
      'role' => 'system',
      'content' => 'You are an assistant synthesizer. Report what the agents actually did — completed actions, errors, and approval pauses. Do not propose plans, ask clarifying questions, or describe what agents could do next. Use only facts present in the execution output. If a step is paused for approval, say exactly which action needs approval and nothing else. Be brief.',
    ],
    [
      'role' => 'user',
      'content' => json_encode([
        'user_message' => (string) $message,
        'workflow_plan' => !empty($plan['agent_ids']) && is_array($plan['agent_ids']) ? array_values($plan['agent_ids']) : [],
        'workflow_results' => ai_assistants_workflow_results_payload($results),
      ]),
    ],
  ];

  try {
    $answer = ai_chat($model, $synthesis_messages, 0.0, 512, FALSE, [
      'operation' => 'ai_assistants_synthesis',
    ]);
    $answer = is_string($answer) ? trim($answer) : trim((string) $answer);
    return $answer !== '' ? $answer : $fallback;
  }
  catch (\Exception $e) {
    watchdog('ai_assistants', 'Workflow synthesis failed, falling back to raw results: @error', ['@error' => $e->getMessage()], WATCHDOG_WARNING);
    return $fallback;
  }
}

/**
 * Generic completion check for one workflow node's result.
 *
 * A node is satisfied when status is ok and the agent produced either an
 * answer or at least one tool call. Per-agent stricter validation can be
 * implemented via custom produces callbacks (the orchestrator collects
 * outputs which the synthesizer surfaces, so missing outputs already show
 * up to the user).
 */
function ai_assistants_workflow_node_satisfied(array $result) {
  if (!empty($result['error']) || (!empty($result['status']) && $result['status'] !== 'ok')) {
    return FALSE;
  }
  return !empty($result['answer']) || !empty($result['tool_calls']);
}

/**
 * Sum loop counts from agent results.
 */
function ai_assistants_sum_result_loops(array $results) {
  return array_sum(array_map(function ($result) {
    return !empty($result['loops']) ? (int) $result['loops'] : 0;
  }, $results));
}

/**
 * Collect tool calls from agent results.
 */
function ai_assistants_collect_result_tool_calls(array $results) {
  $tool_calls = [];
  foreach ($results as $result) {
    if (empty($result['tool_calls']) || !is_array($result['tool_calls'])) {
      continue;
    }
    foreach ($result['tool_calls'] as $tool_call) {
      $tool_call['agent_id'] = $result['agent_id'];
      $tool_calls[] = $tool_call;
    }
  }

  return $tool_calls;
}

/**
 * Public API: send a message to an assistant.
 */
function ai_assistants_send($assistant_id, $message, array $options = []) {
  $assistant = ai_assistants_load($assistant_id);
  if (empty($assistant)) {
    return [
      'answer' => '',
      'structured' => NULL,
      'status' => 'error',
      'token' => NULL,
      'pending' => NULL,
      'thread_id' => '',
      'loops' => 0,
      'tool_calls' => [],
      'error' => "Unknown assistant: $assistant_id",
    ];
  }

  $access_error = ai_assistants_access_error($assistant);
  if ($access_error !== '') {
    return [
      'answer' => '',
      'structured' => NULL,
      'status' => 'error',
      'token' => NULL,
      'pending' => NULL,
      'thread_id' => '',
      'loops' => 0,
      'tool_calls' => [],
      'error' => $access_error,
    ];
  }

  $storage = !empty($assistant['thread_storage']) ? (string) $assistant['thread_storage'] : 'session';
  $thread_id = !empty($options['thread_id']) ? (string) $options['thread_id'] : ai_assistants_create_thread_id($storage);
  $thread = ai_assistants_get_thread($thread_id);
  $messages = !empty($thread['messages']) && is_array($thread['messages']) ? $thread['messages'] : [];

  $context = [
    'assistant' => $assistant,
    'thread_id' => $thread_id,
    'options' => $options,
  ];
  backdrop_alter('ai_assistants_messages', $messages, $assistant_id, $context);

  $route_mode = ai_assistants_get_route_mode($assistant);
  $agent_ids = ai_assistants_get_agent_ids($assistant);
  $agent_options = $options;
  unset($agent_options['thread_id']);

  $response = [
    'answer' => '',
    'structured' => NULL,
    'status' => 'error',
    'token' => NULL,
    'pending' => NULL,
    'thread_id' => $thread_id,
    'loops' => 0,
    'tool_calls' => [],
    'error' => NULL,
  ];

  // This is the runtime branch point: direct answer, one backing agent, or a
  // planned multi-agent workflow all start from the resolved route mode here.
  if ($route_mode === '') {
    $response['error'] = t('No route mode is configured for this assistant.');
  }
  elseif ($route_mode === 'direct') {
    $direct_messages = ai_assistants_build_chat_messages($assistant, $messages, $message);
    try {
      $model = ai_assistants_default_model($assistant);
      if ($model === '') {
        throw new Exception(t('No chat model is configured for this assistant.'));
      }
      $answer = ai_chat($model, $direct_messages, !empty($assistant['temperature']) ? (float) $assistant['temperature'] : 0.3, !empty($assistant['max_tokens']) ? (int) $assistant['max_tokens'] : 1024, FALSE, [
        'operation' => 'ai_assistants',
        'assistant_id' => $assistant_id,
      ]);
      $response['answer'] = is_string($answer) ? $answer : (string) $answer;
      $response['status'] = 'ok';
    }
    catch (\Exception $e) {
      $response['error'] = $e->getMessage();
    }
  }
  elseif (!module_exists('ai_agents')) {
    $response['error'] = t('AI Agent module is required for routed assistants.');
  }
  elseif ($route_mode === 'router') {
    $plan = ai_assistants_plan_agent_execution($assistant, $message, $messages, 'router');
    if (empty($plan['agent_ids']) && ai_assistants_is_confirmation_message($message)) {
      $previous_message = ai_assistants_last_actionable_user_message($messages);
      if ($previous_message !== '') {
        $plan = ai_assistants_plan_agent_execution($assistant, $previous_message, $messages, 'router');
      }
    }
    if (empty($plan['agent_ids'])) {
      $response['error'] = t('No backing agent matches the requested action.');
    }
    else {
      $response = array_merge($response, ai_assistants_execute_agent_plan($assistant_id, $assistant, $messages, $message, $plan, $agent_options));
    }
  }
  elseif ($route_mode === 'multi_agent') {
    $plan = ai_assistants_plan_agent_execution($assistant, $message, $messages, 'multi_agent');
    if (empty($plan['agent_ids'])) {
      $response['error'] = t('Multi-agent mode could not find a relevant agent for this request.');
      ai_assistants_save_thread($thread_id, $assistant_id, array_merge($messages, [
        [
          'role' => 'user',
          'content' => (string) $message,
        ],
      ]), $assistant, $options);
      module_invoke_all('ai_assistants_response', $response, $assistant_id);
      return $response;
    }
    $response = array_merge($response, ai_assistants_execute_agent_plan($assistant_id, $assistant, $messages, $message, $plan, $agent_options));
  }
  elseif ($route_mode === 'single_agent') {
    $agent_id = !empty($assistant['agent_id']) ? (string) $assistant['agent_id'] : '';
    if ($agent_id === FALSE || $agent_id === NULL || $agent_id === '') {
      $response['error'] = t('No backing agent is configured for this assistant.');
    }
    else {
      $runtime_message = ai_assistants_build_agent_message($assistant, $messages, $message);
      $result = ai_assistants_run_backing_agent($assistant_id, $assistant, $agent_id, $runtime_message, $agent_options);
      $response['answer'] = $result['answer'];
      $response['structured'] = $result['structured'];
      $response['status'] = $result['status'];
      $response['token'] = $result['token'];
      $response['pending'] = $result['pending'];
      $response['loops'] = $result['loops'];
      $response['tool_calls'] = $result['tool_calls'];
      $response['error'] = $result['error'];
      $response['agent_id'] = $agent_id;
      $response['agent_label'] = $result['agent_label'];
    }
  }
  else {
    $response['error'] = t('Unsupported route mode: @mode', ['@mode' => $route_mode]);
  }

  if (!empty($response['error']) && empty($response['status'])) {
    $response['status'] = 'error';
  }

  // Persist the routing plan when an approval pause occurs so that resume can
  // replay the same plan instead of re-running the (non-deterministic) router.
  // Only relevant for routed modes (router/multi_agent) where $plan exists.
  if (isset($plan) && $response['status'] === 'needs_approval' && !empty($response['token'])) {
    ai_assistants_set_pending_plan($thread_id, [
      'plan' => $plan,
      'message' => (string) $message,
      'thread_messages' => $messages,
      'route_mode' => $route_mode,
    ]);
  }
  elseif ($response['status'] === 'ok') {
    // Defensive: any successful turn invalidates a stale pending plan.
    ai_assistants_clear_pending_plan($thread_id);
  }

  $thread_messages = $messages;
  $thread_messages[] = [
    'role' => 'user',
    'content' => (string) $message,
  ];
  if ($response['status'] === 'ok' && $response['answer'] !== '') {
    $thread_messages[] = [
      'role' => 'assistant',
      'content' => $response['answer'],
    ];
  }
  ai_assistants_save_thread($thread_id, $assistant_id, $thread_messages, $assistant, $options);

  module_invoke_all('ai_assistants_response', $response, $assistant_id);

  return $response;
}

/**
 * Resume a paused assistant run.
 */
function ai_assistants_resume($assistant_id, $token, $approved = TRUE, array $options = []) {
  $assistant = ai_assistants_load($assistant_id);
  if (empty($assistant)) {
    return [
      'answer' => '',
      'structured' => NULL,
      'status' => 'error',
      'token' => NULL,
      'pending' => NULL,
      'thread_id' => '',
      'loops' => 0,
      'tool_calls' => [],
      'error' => 'Assistant not found.',
    ];
  }

  $access_error = ai_assistants_access_error($assistant);
  if ($access_error !== '') {
    return [
      'answer' => '',
      'structured' => NULL,
      'status' => 'error',
      'token' => NULL,
      'pending' => NULL,
      'thread_id' => '',
      'loops' => 0,
      'tool_calls' => [],
      'error' => $access_error,
    ];
  }

  $agent_options = $options;
  $agent_options['resume_token'] = $token;
  $agent_options['approved'] = (bool) $approved;

  $route_mode = ai_assistants_get_route_mode($assistant);
  $agent_id = !empty($options['agent_id']) ? (string) $options['agent_id'] : '';
  if ($agent_id === '') {
    return [
      'answer' => '',
      'structured' => NULL,
      'status' => 'error',
      'token' => NULL,
      'pending' => NULL,
      'thread_id' => !empty($options['thread_id']) ? (string) $options['thread_id'] : '',
      'loops' => 0,
      'tool_calls' => [],
      'error' => t('No backing agent id was supplied for this request.'),
    ];
  }

  if (!function_exists('ai_agents_resume_run')) {
    return [
      'answer' => '',
      'structured' => NULL,
      'status' => 'error',
      'token' => NULL,
      'pending' => NULL,
      'thread_id' => !empty($options['thread_id']) ? (string) $options['thread_id'] : '',
      'loops' => 0,
      'tool_calls' => [],
      'error' => t('Approval resume is not available.'),
    ];
  }

  $result = ai_agents_resume_run($agent_id, $agent_options);
  if (is_string($result)) {
    $result = [
      'answer' => '',
      'loops' => 0,
      'tool_calls' => [],
      'error' => 'Unexpected async job response from agent runner.',
    ];
  }

  return [
    'answer' => isset($result['answer']) ? (string) $result['answer'] : '',
    'structured' => isset($result['structured']) ? $result['structured'] : NULL,
    'status' => !empty($result['status']) ? (string) $result['status'] : (empty($result['error']) ? 'ok' : 'error'),
    'token' => isset($result['token']) ? (string) $result['token'] : NULL,
    'pending' => isset($result['pending_tool']) ? $result['pending_tool'] : NULL,
    'thread_id' => !empty($options['thread_id']) ? (string) $options['thread_id'] : '',
    'loops' => isset($result['loops']) ? (int) $result['loops'] : 0,
    'tool_calls' => isset($result['tool_calls']) && is_array($result['tool_calls']) ? $result['tool_calls'] : [],
    'error' => isset($result['error']) ? $result['error'] : NULL,
  ];
}

/**
 * Continues routed assistant work after one backing agent approval resumes.
 */
function ai_assistants_continue_after_approval($assistant_id, array $assistant, array $approved_result, $approved_agent_id, $thread_id, array $options = []) {
  $route_mode = ai_assistants_get_route_mode($assistant);
  if (!in_array($route_mode, ['router', 'multi_agent'], TRUE)) {
    return $approved_result;
  }

  $approved_agent_id = (string) $approved_agent_id;
  if ($approved_agent_id === '' || !empty($approved_result['error']) || (!empty($approved_result['status']) && $approved_result['status'] !== 'ok')) {
    return $approved_result;
  }

  $thread = ai_assistants_get_thread((string) $thread_id);
  $messages = !empty($thread['messages']) && is_array($thread['messages']) ? $thread['messages'] : [];

  // Replay the plan that was active when the pause occurred. Re-running the
  // LLM router here would be non-deterministic and could pick a different
  // agent set than the one the user was already mid-approval on.
  $pending = ai_assistants_get_pending_plan((string) $thread_id);
  if (!empty($pending['plan']) && is_array($pending['plan'])) {
    $plan = $pending['plan'];
    $message = !empty($pending['message']) ? (string) $pending['message'] : ai_assistants_last_actionable_user_message($messages);
    if (!empty($pending['thread_messages']) && is_array($pending['thread_messages'])) {
      $messages = $pending['thread_messages'];
    }
  }
  else {
    $message = ai_assistants_last_actionable_user_message($messages);
    if ($message === '') {
      return $approved_result;
    }
    $plan = ai_assistants_plan_agent_execution($assistant, $message, $messages, $route_mode);
  }
  if ($message === '') {
    return $approved_result;
  }
  if (!empty($plan['workflow']) && is_array($plan['workflow'])) {
    $workflow = array_values($plan['workflow']);
    $approved_index = FALSE;
    foreach ($workflow as $index => $node) {
      if (!empty($node['agent_id']) && $node['agent_id'] === $approved_agent_id) {
        $approved_index = $index;
        break;
      }
    }
    if ($approved_index === FALSE || $approved_index >= count($workflow) - 1) {
      return $approved_result;
    }

    $approved_agent = ai_agents_load($approved_agent_id);
    $approved_label = !empty($approved_agent['label']) ? $approved_agent['label'] : $approved_agent_id;
    $seed_results = [[
      'assistant_id' => $assistant_id,
      'agent_id' => $approved_agent_id,
      'agent_label' => $approved_label,
      'answer' => !empty($approved_result['answer']) ? (string) $approved_result['answer'] : '',
      'structured' => isset($approved_result['structured']) ? $approved_result['structured'] : NULL,
      'status' => !empty($approved_result['status']) ? (string) $approved_result['status'] : 'ok',
      'token' => NULL,
      'pending' => NULL,
      'loops' => !empty($approved_result['loops']) ? (int) $approved_result['loops'] : 0,
      'tool_calls' => !empty($approved_result['tool_calls']) && is_array($approved_result['tool_calls']) ? $approved_result['tool_calls'] : [],
      'error' => NULL,
    ]];

    $remaining_plan = $plan;
    $remaining_plan['workflow'] = array_slice($workflow, $approved_index + 1);
    $remaining_plan['agent_ids'] = array_values(array_unique(array_map(function ($node) {
      return $node['agent_id'];
    }, $remaining_plan['workflow'])));

    $agent_options = $options;
    unset($agent_options['thread_id'], $agent_options['agent_id'], $agent_options['resume_token'], $agent_options['approved']);
    return ai_assistants_execute_workflow_plan($assistant_id, $assistant, $message, $remaining_plan, $agent_options, $seed_results);
  }

  $agent_ids = !empty($plan['agent_ids']) && is_array($plan['agent_ids']) ? $plan['agent_ids'] : [];
  if (empty($agent_ids) || empty($plan['strategy']) || $plan['strategy'] !== 'ask_many') {
    return $approved_result;
  }

  $approved_position = array_search($approved_agent_id, $agent_ids, TRUE);
  if ($approved_position === FALSE) {
    return $approved_result;
  }

  $remaining_agent_ids = array_slice($agent_ids, $approved_position + 1);
  if (empty($remaining_agent_ids)) {
    return $approved_result;
  }

  $approved_agent = ai_agents_load($approved_agent_id);
  $approved_label = !empty($approved_agent['label']) ? $approved_agent['label'] : $approved_agent_id;
  $results = [[
    'assistant_id' => $assistant_id,
    'agent_id' => $approved_agent_id,
    'agent_label' => $approved_label,
    'answer' => !empty($approved_result['answer']) ? (string) $approved_result['answer'] : '',
    'structured' => isset($approved_result['structured']) ? $approved_result['structured'] : NULL,
    'status' => !empty($approved_result['status']) ? (string) $approved_result['status'] : 'ok',
    'token' => NULL,
    'pending' => NULL,
    'loops' => !empty($approved_result['loops']) ? (int) $approved_result['loops'] : 0,
    'tool_calls' => !empty($approved_result['tool_calls']) && is_array($approved_result['tool_calls']) ? $approved_result['tool_calls'] : [],
    'error' => NULL,
  ]];

  $agent_options = $options;
  unset($agent_options['thread_id'], $agent_options['agent_id'], $agent_options['resume_token'], $agent_options['approved']);
  $base_runtime_message = ai_assistants_build_agent_message($assistant, $messages, $message);
  $isolated_runtime_message = ai_assistants_build_agent_message($assistant, [], $message);

  $combined = [
    'answer' => '',
    'structured' => NULL,
    'status' => 'ok',
    'token' => NULL,
    'pending' => NULL,
    'thread_id' => (string) $thread_id,
    'loops' => !empty($approved_result['loops']) ? (int) $approved_result['loops'] : 0,
    'tool_calls' => !empty($approved_result['tool_calls']) && is_array($approved_result['tool_calls']) ? $approved_result['tool_calls'] : [],
    'error' => NULL,
  ];

  foreach ($remaining_agent_ids as $agent_id) {
    $agent_id = (string) $agent_id;
    $info = ai_assistants_orchestration_info($agent_id);
    // Consumer agents get the isolated message so prior transcript context
    // cannot leak past the explicit produced inputs.
    $runtime_message = !empty($info['consumes']) ? $isolated_runtime_message : $base_runtime_message;
    $outputs = ai_assistants_orchestration_collect_outputs($results);
    $runtime_message = ai_assistants_orchestration_inject($agent_id, $runtime_message, $outputs);
    $result = ai_assistants_run_backing_agent($assistant_id, $assistant, $agent_id, $runtime_message, $agent_options);
    $results[] = $result;
    $combined['loops'] += !empty($result['loops']) ? (int) $result['loops'] : 0;
    foreach ($result['tool_calls'] as $tool_call) {
      $tool_call['agent_id'] = $result['agent_id'];
      $combined['tool_calls'][] = $tool_call;
    }

    if ($result['status'] === 'needs_approval') {
      $combined['status'] = 'needs_approval';
      $combined['token'] = $result['token'];
      $combined['pending'] = $result['pending'];
      $combined['pending_agent_id'] = $agent_id;
      $combined['agent_id'] = $agent_id;
      $combined['agent_label'] = $result['agent_label'];
      $combined['answer'] = ai_assistants_format_multi_agent_answer($results);
      return $combined;
    }
  }

  $combined['answer'] = ai_assistants_format_multi_agent_answer($results);
  $combined['status'] = !empty($combined['answer']) ? 'ok' : 'error';
  if ($combined['status'] === 'error') {
    if (empty($combined['error'])) {
      $combined['error'] = t('No backing agents returned a response.');
    }
  }

  return $combined;
}
