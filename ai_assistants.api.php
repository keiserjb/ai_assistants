<?php

/**
 * @file
 * Hooks provided by the AI Assistants module.
 */

/**
 * @defgroup ai_assistants_hooks AI Assistants hooks
 * @{
 */

/**
 * Alter the messages array before an assistant call is sent to the model.
 *
 * @param array $messages
 *   The thread message history that will be sent to the assistant. Modify in
 *   place to inject persona prompts, content guards, or extra context.
 * @param string $assistant_id
 *   The machine name of the assistant being invoked.
 * @param array $context
 *   Additional context: 'assistant' (config), 'thread_id', 'options'.
 */
function hook_ai_assistants_messages_alter(array &$messages, $assistant_id, array $context) {
  $messages[] = [
    'role' => 'system',
    'content' => 'Always cite sources when available.',
  ];
}

/**
 * React to an assistant response after it has been finalized.
 *
 * @param array $response
 *   The full response array returned to the caller.
 * @param string $assistant_id
 *   The machine name of the assistant that produced the response.
 */
function hook_ai_assistants_response(array $response, $assistant_id) {
  if (!empty($response['error'])) {
    watchdog('mymodule', 'Assistant @id error: @msg', [
      '@id' => $assistant_id,
      '@msg' => $response['error'],
    ], WATCHDOG_NOTICE);
  }
}

/**
 * Declare orchestration callbacks for one or more agents.
 *
 * Each agent can declare:
 *   - produces: map of output_key => callable($result_array): mixed.
 *     Invoked after the agent runs. The callable receives the agent's
 *     normalized result and returns a value (or empty if nothing produced).
 *   - consumes: map of input_key => callable($runtime_message, $value): string.
 *     Invoked before the agent runs, once per matching upstream output. The
 *     callable returns the modified runtime message.
 *
 * Used by ai_assistants when the router picks more than one agent. Outputs
 * collected from earlier agents in the run are piped into later agents that
 * declare a matching consumes key.
 *
 * Static metadata (priority, depends_on) is stored on each agent's config
 * record and edited via the agent admin form, not via this hook.
 *
 * @return array
 *   Map of agent_id => ['produces' => [...], 'consumes' => [...]].
 *
 * @see ai_assistants_orchestration_info()
 * @see ai_assistants_orchestration_collect_outputs()
 * @see ai_assistants_orchestration_inject()
 */
function hook_ai_assistants_agent_orchestration_callbacks() {
  return [
    'product_agent' => [
      'produces' => [
        'product_id' => 'mymodule_extract_created_product_id',
      ],
    ],
    'inventory_agent' => [
      'consumes' => [
        'product_id' => 'mymodule_inject_product_id_context',
      ],
    ],
  ];
}

/**
 * Alter the merged orchestration callbacks map.
 *
 * Lets sites override or remove the produces/consumes callbacks contributed
 * by other modules — for instance, to disable a default registration when
 * swapping out the agent set.
 *
 * @param array $callbacks
 *   Merged map of agent_id => ['produces' => [...], 'consumes' => [...]].
 */
function hook_ai_assistants_agent_orchestration_callbacks_alter(array &$callbacks) {
  unset($callbacks['content_type_agent']);
}

/**
 * @}
 */
