<?php

/**
 * @file
 * Assistant routing and planning helpers.
 *
 * The router asks an LLM which configured agents should handle one user
 * message and which strategy to use. The orchestrator (see
 * ai_assistants.orchestrator.inc) honors per-agent declared metadata
 * (priority, depends_on, produces, consumes) when ordering and piping the
 * results — none of that knowledge lives here.
 */

/**
 * Build an explicit execution plan for one user message.
 *
 * Returns:
 *   strategy : 'choose_one' | 'ask_many' | 'workflow'
 *   agent_ids: ordered list of agent ids to run
 *   reason   : optional model-supplied rationale
 */
function ai_assistants_plan_agent_execution(array $assistant, $message, array $thread_messages = [], $route_mode = '') {
  $route_mode = $route_mode !== '' ? (string) $route_mode : ai_assistants_get_route_mode($assistant);
  $configured = ai_assistants_get_agent_ids($assistant);
  if (empty($configured)) {
    return [
      'strategy' => 'choose_one',
      'agent_ids' => [],
      'reason' => 'no-agents-configured',
    ];
  }

  $planned = ai_assistants_model_route_plan($assistant, $message, $thread_messages);

  if (is_array($planned)) {
    $raw_ids = isset($planned['raw_agent_ids']) && is_array($planned['raw_agent_ids'])
      ? $planned['raw_agent_ids']
      : (!empty($planned['agent_ids']) && is_array($planned['agent_ids']) ? $planned['agent_ids'] : []);
    $selected = array_values(array_unique(array_intersect($raw_ids, $configured)));
    if (empty($selected)) {
      if (empty($raw_ids)) {
        // The router deliberately returned an empty list: a "no agent matches"
        // verdict. Honor it instead of fanning out to every agent.
        return [
          'strategy' => 'choose_one',
          'agent_ids' => [],
          'reason' => !empty($planned['reason']) ? (string) $planned['reason'] : 'router-no-match',
        ];
      }
      // The router returned IDs but all were filtered out as unconfigured.
      // Fall back to all configured agents rather than returning an empty plan.
      $selected = $configured;
    }
  }
  else {
    // LLM router unavailable (no model, call failed, or unparseable output):
    // send to all configured agents and let each decide whether the message
    // is for it.
    $selected = $configured;
  }

  $strategy = !empty($planned['strategy']) ? ai_assistants_normalize_route_strategy($planned['strategy']) : '';
  if ($strategy === '') {
    $strategy = count($selected) > 1 ? 'ask_many' : 'choose_one';
  }

  if ($strategy === 'choose_one' && !empty($selected)) {
    $selected = [reset($selected)];
  }
  else {
    // Apply orchestration ordering so declared dependencies are honored even
    // when the LLM router put the consumer before the producer.
    $selected = ai_assistants_orchestration_sort($selected);
  }

  $plan = [
    'strategy' => $strategy,
    'agent_ids' => $selected,
    'reason' => !empty($planned['reason']) ? (string) $planned['reason'] : '',
  ];
  if ($strategy === 'workflow') {
    // Workflow execution iterates a list of nodes. Each node is just an agent
    // — context piping is handled by orchestrator produces/consumes callbacks.
    $plan['workflow'] = array_map(function ($agent_id) {
      return ['agent_id' => (string) $agent_id];
    }, $selected);
  }
  return $plan;
}

/**
 * Choose agents with an LLM planner using configured agent metadata.
 *
 * The router prompt receives each agent's description, system prompt summary,
 * enabled tools, and declared orchestration metadata (priority, depends_on,
 * produces, consumes). The model returns one of three strategies:
 *
 *   choose_one : exactly one agent can handle the full request
 *   ask_many   : multiple agents should each answer the same request
 *   workflow   : multiple agents must run in dependency order with output
 *                from one piped into another (declared via produces/consumes)
 */
/**
 * Returns the default router system prompt template.
 *
 * The template supports placeholders that are filled at runtime by aggregating
 * fields contributed by each available agent:
 *
 *   [examples]            — concatenation of every agent's `routing_examples`
 *                            field (set on the agent admin form). Each agent
 *                            owns its own examples; the router prompt itself
 *                            never names a specific agent.
 *   [usage_instructions]  — concatenation of every agent's
 *                            `routing_usage_instructions` field.
 *
 * Site admins can override the entire template per-assistant via the assistant
 * edit form's "Pre-action prompt" field. The override also supports the same
 * placeholders.
 */
function ai_assistants_default_router_prompt() {
  return 'You are a strict assistant router. Pick the SMALLEST set of configured backing agents that can fulfill the current user request.'
    . "\n\nReturn JSON only with this shape:"
    . "\n{\"strategy\":\"choose_one|ask_many|workflow\",\"agent_ids\":[\"agent_id\"],\"reason\":\"short reason\"}"
    . "\n\nDecision algorithm — apply in order:"
    . "\n1. Identify the request's domain (what part of the site it concerns) and intent: an ACTION (create or change something) or a QUESTION (read, list, explain, or advise)."
    . "\n2. Match the domain against each agent's description and the usage notes below. Some tools carry an optional \"op\" value: \"write\"/\"trigger\" means the tool performs changes; \"read\" means it only looks things up. Tools without an op value should be matched on name and description alone."
    . "\n3. For ACTION requests, prefer agents whose tools include op \"write\" or \"trigger\" in the matching domain (when op is present). For QUESTION or advisory requests, an agent whose tools are all op \"read\" — or have no op — is a valid choice."
    . "\n4. From the matching candidates, pick the FEWEST agents that together cover the request:"
    . "\n   - 1 agent → strategy choose_one."
    . "\n   - 2+ agents whose declared produces/consumes keys form a chain, or whose depends_on metadata wires them together → strategy workflow. Return agent_ids in dependency order."
    . "\n   - 2+ agents where the request has genuinely distinct required parts and no single agent covers them all → strategy ask_many."
    . "\n   - 2+ overlapping candidates competing for the same part of the request → pick the single best fit, strategy choose_one. Do NOT hedge with ask_many."
    . "\n5. If no agent's domain matches the request, return an empty agent_ids array."
    . "\n\nHard rules:"
    . "\n- Use only agent ids that appear in AVAILABLE_AGENTS."
    . "\n- Never select an agent whose domain does not match the request just to return something — an empty agent_ids array is the correct answer for unmatched requests."
    . "\n- Prefer choose_one over ask_many whenever you are unsure."
    . "\n\nUsage notes for the configured agents:"
    . "\n[usage_instructions]"
    . "\n\nFew-shot examples contributed by the configured agents:"
    . "\n[examples]";
}

/**
 * Aggregate per-agent routing examples and usage instructions.
 *
 * Reads each available agent's `routing_examples` and
 * `routing_usage_instructions` config fields and concatenates them. Agents
 * that have nothing to contribute are silently skipped.
 *
 * @return array
 *   ['examples' => string, 'usage_instructions' => string]
 */
function ai_assistants_collect_agent_router_contributions(array $agent_ids) {
  $examples = [];
  $instructions = [];
  foreach ($agent_ids as $agent_id) {
    $agent = function_exists('ai_agents_load') ? ai_agents_load((string) $agent_id) : NULL;
    if (empty($agent)) {
      continue;
    }
    $label = !empty($agent['label']) ? (string) $agent['label'] : (string) $agent_id;
    if (!empty($agent['routing_examples']) && is_string($agent['routing_examples'])) {
      $examples[] = '# ' . $label . ' (' . $agent_id . ")\n" . trim($agent['routing_examples']);
    }
    if (!empty($agent['routing_usage_instructions']) && is_string($agent['routing_usage_instructions'])) {
      $instructions[] = '# ' . $label . ' (' . $agent_id . ")\n" . trim($agent['routing_usage_instructions']);
    }
  }
  return [
    'examples' => empty($examples) ? '(none configured)' : implode("\n\n", $examples),
    'usage_instructions' => empty($instructions) ? '(none configured)' : implode("\n\n", $instructions),
  ];
}

function ai_assistants_model_route_plan(array $assistant, $message, array $thread_messages = []) {
  if (!function_exists('ai_chat')) {
    return NULL;
  }

  $model = ai_assistants_default_model($assistant);
  if ($model === '') {
    return NULL;
  }

  $agent_ids = ai_assistants_get_agent_ids($assistant);
  if (empty($agent_ids) || !function_exists('ai_agents_load')) {
    return NULL;
  }

  $available = ai_assistants_build_router_agent_manifest($agent_ids);
  if (empty($available)) {
    return NULL;
  }

  // Last 4 user/assistant turns, capped at 500 chars each. Router only needs
  // enough context to detect "is this a continuation of an earlier ask".
  // System messages (persona injections, secured prompts) waste tokens here.
  $recent_messages = [];
  for ($i = count($thread_messages) - 1; $i >= 0 && count($recent_messages) < 4; $i--) {
    $entry = $thread_messages[$i];
    if (empty($entry['role']) || !isset($entry['content'])) {
      continue;
    }
    $role = (string) $entry['role'];
    if ($role !== 'user' && $role !== 'assistant') {
      continue;
    }
    array_unshift($recent_messages, [
      'role' => $role,
      'content' => mb_substr((string) $entry['content'], 0, 500),
    ]);
  }

  $template = !empty($assistant['pre_action_prompt'])
    ? (string) $assistant['pre_action_prompt']
    : ai_assistants_default_router_prompt();
  $contributions = ai_assistants_collect_agent_router_contributions($agent_ids);
  $system_prompt = strtr($template, [
    '[examples]' => $contributions['examples'],
    '[usage_instructions]' => $contributions['usage_instructions'],
  ]);
  $system_prompt = ai_assistants_substitute_prompt_tokens($system_prompt);

  $router_messages = [
    [
      'role' => 'system',
      'content' => $system_prompt,
    ],
    [
      'role' => 'user',
      'content' => json_encode([
        'available_agents' => $available,
        'recent_thread_messages' => $recent_messages,
        'user_message' => (string) $message,
      ]),
    ],
  ];

  try {
    // 2048 tokens: reasoning models spend output budget on hidden reasoning
    // before emitting the JSON verdict — 512 was small enough that the
    // verdict could be truncated or empty, which silently disabled routing.
    $answer = ai_chat($model, $router_messages, 0.0, 2048, FALSE, [
      'operation' => 'ai_assistants_router',
    ]);
  }
  catch (\Exception $e) {
    watchdog('ai_assistants', 'Model router failed and will use fallback routing: @error', ['@error' => $e->getMessage()], WATCHDOG_WARNING);
    return NULL;
  }

  $data = ai_assistants_decode_router_json(is_string($answer) ? $answer : json_encode($answer));
  if (!is_array($data) || !isset($data['agent_ids']) || !is_array($data['agent_ids'])) {
    return NULL;
  }

  $raw_agent_ids = [];
  foreach ($data['agent_ids'] as $entry) {
    if (!is_scalar($entry)) {
      continue;
    }
    $entry = trim((string) $entry);
    if ($entry === '') {
      continue;
    }
    $raw_agent_ids[] = $entry;
  }

  $allowed = array_fill_keys(array_keys($available), TRUE);
  $selected = [];
  foreach ($raw_agent_ids as $agent_id) {
    if (isset($allowed[$agent_id]) && !in_array($agent_id, $selected, TRUE)) {
      $selected[] = $agent_id;
    }
  }

  return [
    'strategy' => !empty($data['strategy']) ? ai_assistants_normalize_route_strategy($data['strategy']) : '',
    'agent_ids' => $selected,
    'raw_agent_ids' => $raw_agent_ids,
    'reason' => !empty($data['reason']) ? (string) $data['reason'] : '',
  ];
}

/**
 * Normalize a router execution strategy.
 */
function ai_assistants_normalize_route_strategy($strategy) {
  $strategy = trim((string) $strategy);
  return in_array($strategy, ['choose_one', 'ask_many', 'workflow'], TRUE) ? $strategy : '';
}

/**
 * Builds compact agent metadata for router planning.
 */
function ai_assistants_build_router_agent_manifest(array $agent_ids) {
  $tools = function_exists('ai_tools_get_tools') ? ai_tools_get_tools() : [];
  $orchestration_manifest = ai_assistants_orchestration_manifest($agent_ids);
  $manifest = [];

  foreach ($agent_ids as $agent_id) {
    $agent_id = (string) $agent_id;
    $agent = ai_agents_load($agent_id);
    if (empty($agent)) {
      continue;
    }

    // Compact tool list: name + operation only. Names already encode action
    // (create_/modify_/...) vs read-only (get_/list_/...) for the router. Tool
    // descriptions are agent-internal and don't help the routing decision.
    $tool_summaries = [];
    $enabled_tools = !empty($agent['tools']) && is_array($agent['tools']) ? array_values(array_filter($agent['tools'])) : [];
    foreach (array_slice($enabled_tools, 0, 16) as $tool_id) {
      $tool_id = (string) $tool_id;
      $entry = ['name' => $tool_id];
      if (!empty($tools[$tool_id]['operation'])) {
        $entry['op'] = (string) $tools[$tool_id]['operation'];
      }
      $tool_summaries[] = $entry;
    }

    $orchestration = isset($orchestration_manifest[$agent_id]) ? $orchestration_manifest[$agent_id] : [
      'depends_on' => [],
      'produces' => [],
      'consumes' => [],
    ];

    // Manifest stays small: description + compact tools + orchestration only.
    // The agent's system_prompt is for its own runs, not for routing — the
    // router prompt explicitly tells the model to read description and tools.
    $manifest[$agent_id] = [
      'id' => $agent_id,
      'label' => !empty($agent['label']) ? (string) $agent['label'] : $agent_id,
      'description' => !empty($agent['description']) ? (string) $agent['description'] : '',
      'tools' => $tool_summaries,
      'depends_on' => $orchestration['depends_on'],
      'produces' => $orchestration['produces'],
      'consumes' => $orchestration['consumes'],
    ];
  }

  return $manifest;
}

/**
 * Decode a router JSON response, including fenced or prefixed JSON.
 */
function ai_assistants_decode_router_json($answer) {
  $answer = trim((string) $answer);
  if ($answer === '') {
    return NULL;
  }

  $data = json_decode($answer, TRUE);
  if (json_last_error() === JSON_ERROR_NONE && is_array($data)) {
    return $data;
  }

  if (preg_match('/```(?:json)?\s*(.+?)\s*```/is', $answer, $matches)) {
    $data = json_decode(trim($matches[1]), TRUE);
    if (json_last_error() === JSON_ERROR_NONE && is_array($data)) {
      return $data;
    }
  }

  if (preg_match('/\{.*\}/s', $answer, $matches)) {
    $data = json_decode($matches[0], TRUE);
    if (json_last_error() === JSON_ERROR_NONE && is_array($data)) {
      return $data;
    }
  }

  return NULL;
}

/**
 * Returns TRUE when a message contains any keyword.
 */
function ai_assistants_message_contains_any($message, array $needles) {
  foreach ($needles as $needle) {
    if (strpos($message, $needle) !== FALSE) {
      return TRUE;
    }
  }
  return FALSE;
}

/**
 * Returns TRUE if a message is a short confirmation of prior context.
 */
function ai_assistants_is_confirmation_message($message) {
  $message = backdrop_strtolower(trim((string) $message));
  if ($message === '') {
    return FALSE;
  }

  $confirmations = [
    'yes',
    'y',
    'ok',
    'okay',
    'sure',
    'proceed',
    'go ahead',
    'do it',
    'please do',
    'confirmed',
    'confirm',
    'continue',
  ];

  return in_array($message, $confirmations, TRUE);
}

/**
 * Returns TRUE if a message is an explicit approval of a pending action.
 *
 * Broader than is_confirmation_message — includes "approve" and variants so
 * that a user can type approval text to resume a paused agent run.
 */
function ai_assistants_is_approval_text($message) {
  $message = backdrop_strtolower(trim((string) $message));
  if ($message === '') {
    return FALSE;
  }
  $approvals = [
    'approve',
    'approved',
    'yes',
    'y',
    'ok',
    'okay',
    'sure',
    'proceed',
    'go ahead',
    'do it',
    'please do',
    'confirmed',
    'confirm',
    'continue',
    'yes please',
    'yes do it',
  ];
  return in_array($message, $approvals, TRUE);
}

/**
 * Finds the most recent substantive user request in a thread.
 */
function ai_assistants_last_actionable_user_message(array $messages) {
  for ($i = count($messages) - 1; $i >= 0; $i--) {
    $entry = $messages[$i];
    if (empty($entry['role']) || $entry['role'] !== 'user' || !isset($entry['content'])) {
      continue;
    }
    $content = trim((string) $entry['content']);
    if ($content === '' || ai_assistants_is_confirmation_message($content)) {
      continue;
    }
    return $content;
  }

  return '';
}
