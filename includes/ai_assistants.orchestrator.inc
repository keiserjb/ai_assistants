<?php

/**
 * @file
 * Generic agent orchestration helpers.
 *
 * Agents declare two kinds of orchestration metadata:
 *   1. Static metadata stored on the agent config record:
 *      - priority   (int, default 100, lower = runs first in ask_many)
 *      - depends_on (array of agent_ids, must run after these)
 *
 *   2. Runtime callbacks declared via
 *      hook_ai_assistants_agent_orchestration_callbacks():
 *      - produces[$key] => callable($result_array): mixed
 *        Extracts a named output from one agent's result.
 *      - consumes[$key] => callable($runtime_message, $value): string
 *        Injects a named input into an agent's runtime message.
 *
 * The orchestrator wires producers to consumers across an ordered agent run,
 * so no agent_id, content type, field, or taxonomy logic appears here.
 */

/**
 * Returns combined orchestration info for one agent.
 *
 * Merges static config (priority/depends_on) with hook-supplied callbacks
 * (produces/consumes). Returns a normalized array even if the agent declares
 * nothing.
 */
function ai_assistants_orchestration_info($agent_id) {
  $cache = &backdrop_static(__FUNCTION__, []);
  $agent_id = (string) $agent_id;
  if ($agent_id === '') {
    return ai_assistants_orchestration_default_info();
  }
  if (!array_key_exists($agent_id, $cache)) {
    $info = ai_assistants_orchestration_default_info();

    if (function_exists('ai_agents_load')) {
      $agent = ai_agents_load($agent_id);
      if (!empty($agent)) {
        if (isset($agent['priority']) && is_numeric($agent['priority'])) {
          $info['priority'] = (int) $agent['priority'];
        }
        if (!empty($agent['depends_on']) && is_array($agent['depends_on'])) {
          $info['depends_on'] = array_values(array_filter(array_map('strval', $agent['depends_on'])));
        }
      }
    }

    $callbacks = ai_assistants_orchestration_callbacks();
    if (!empty($callbacks[$agent_id]['produces']) && is_array($callbacks[$agent_id]['produces'])) {
      foreach ($callbacks[$agent_id]['produces'] as $key => $callable) {
        if (is_callable($callable)) {
          $info['produces'][(string) $key] = $callable;
        }
      }
    }
    if (!empty($callbacks[$agent_id]['consumes']) && is_array($callbacks[$agent_id]['consumes'])) {
      foreach ($callbacks[$agent_id]['consumes'] as $key => $callable) {
        if (is_callable($callable)) {
          $info['consumes'][(string) $key] = $callable;
        }
      }
    }

    $cache[$agent_id] = $info;
  }
  return $cache[$agent_id];
}

/**
 * Default orchestration info shape.
 */
function ai_assistants_orchestration_default_info() {
  return [
    'priority' => 100,
    'depends_on' => [],
    'produces' => [],
    'consumes' => [],
  ];
}

/**
 * Returns the merged hook map: agent_id => [produces => [], consumes => []].
 */
function ai_assistants_orchestration_callbacks() {
  $callbacks = &backdrop_static(__FUNCTION__);
  if (isset($callbacks)) {
    return $callbacks;
  }
  $callbacks = [];
  foreach (module_implements('ai_assistants_agent_orchestration_callbacks') as $module) {
    $impl = module_invoke($module, 'ai_assistants_agent_orchestration_callbacks');
    if (!is_array($impl)) {
      continue;
    }
    foreach ($impl as $agent_id => $entry) {
      $agent_id = (string) $agent_id;
      if (!is_array($entry)) {
        continue;
      }
      if (!isset($callbacks[$agent_id])) {
        $callbacks[$agent_id] = ['produces' => [], 'consumes' => []];
      }
      if (!empty($entry['produces']) && is_array($entry['produces'])) {
        $callbacks[$agent_id]['produces'] = array_replace($callbacks[$agent_id]['produces'], $entry['produces']);
      }
      if (!empty($entry['consumes']) && is_array($entry['consumes'])) {
        $callbacks[$agent_id]['consumes'] = array_replace($callbacks[$agent_id]['consumes'], $entry['consumes']);
      }
    }
  }
  backdrop_alter('ai_assistants_agent_orchestration_callbacks', $callbacks);
  return $callbacks;
}

/**
 * Sort agent IDs by declared priority then topologically by depends_on.
 *
 * Preserves the LLM router's relative order when no priority/dependency
 * conflict requires re-ordering. Cycles are broken by falling back to the
 * input order.
 */
function ai_assistants_orchestration_sort(array $agent_ids) {
  $agent_ids = array_values(array_unique(array_filter(array_map('strval', $agent_ids))));
  if (count($agent_ids) <= 1) {
    return $agent_ids;
  }

  // Stable priority sort first (lower priority = earlier).
  $indexed = [];
  foreach ($agent_ids as $position => $agent_id) {
    $info = ai_assistants_orchestration_info($agent_id);
    $indexed[] = [
      'agent_id' => $agent_id,
      'priority' => (int) $info['priority'],
      'position' => $position,
    ];
  }
  usort($indexed, function ($a, $b) {
    if ($a['priority'] === $b['priority']) {
      return $a['position'] - $b['position'];
    }
    return $a['priority'] - $b['priority'];
  });
  $sorted = array_map(function ($entry) {
    return $entry['agent_id'];
  }, $indexed);

  // Topological pass over depends_on. If A depends_on B and A is currently
  // before B in the list, move A after B.
  $present = array_fill_keys($sorted, TRUE);
  $ordered = [];
  $remaining = $sorted;
  $guard = count($remaining) * count($remaining) + 1;
  while (!empty($remaining) && $guard-- > 0) {
    $agent_id = array_shift($remaining);
    $info = ai_assistants_orchestration_info($agent_id);
    $unmet = [];
    foreach ($info['depends_on'] as $dep) {
      if (!empty($present[$dep]) && !in_array($dep, $ordered, TRUE)) {
        $unmet[] = $dep;
      }
    }
    if (empty($unmet)) {
      $ordered[] = $agent_id;
      continue;
    }
    // Push back to the end so dependencies can land first.
    $remaining[] = $agent_id;
  }
  if (!empty($remaining)) {
    // Cycle or unresolvable: append remaining in original order.
    foreach ($remaining as $agent_id) {
      if (!in_array($agent_id, $ordered, TRUE)) {
        $ordered[] = $agent_id;
      }
    }
  }
  return $ordered;
}

/**
 * Collect produced values from a list of result arrays.
 *
 * Returns ['key' => value, ...] for every produces callback that returned a
 * non-empty value across the supplied results. Later results overwrite earlier
 * results for the same key.
 */
function ai_assistants_orchestration_collect_outputs(array $results) {
  $outputs = [];
  foreach ($results as $result) {
    if (empty($result['agent_id'])) {
      continue;
    }
    $info = ai_assistants_orchestration_info($result['agent_id']);
    foreach ($info['produces'] as $key => $callable) {
      $value = call_user_func($callable, $result);
      if ($value === NULL || $value === '' || $value === [] || $value === FALSE) {
        continue;
      }
      $outputs[$key] = $value;
    }
  }
  return $outputs;
}

/**
 * Apply consume callbacks to a runtime message using collected outputs.
 *
 * For each key the agent consumes that exists in $outputs, the consume
 * callback is invoked to mutate the runtime message with that value.
 */
function ai_assistants_orchestration_inject($agent_id, $runtime_message, array $outputs) {
  $info = ai_assistants_orchestration_info($agent_id);
  if (empty($info['consumes'])) {
    return $runtime_message;
  }
  foreach ($info['consumes'] as $key => $callable) {
    if (!array_key_exists($key, $outputs)) {
      continue;
    }
    $next = call_user_func($callable, $runtime_message, $outputs[$key]);
    if (is_string($next)) {
      $runtime_message = $next;
    }
  }
  return $runtime_message;
}

/**
 * Returns a compact orchestration manifest for the LLM router prompt.
 *
 * Lets the model see declared dependencies and produced outputs so it can
 * order agents correctly without us hardcoding the relationships.
 */
function ai_assistants_orchestration_manifest(array $agent_ids) {
  $manifest = [];
  foreach ($agent_ids as $agent_id) {
    $info = ai_assistants_orchestration_info($agent_id);
    $manifest[$agent_id] = [
      'priority' => (int) $info['priority'],
      'depends_on' => array_values($info['depends_on']),
      'produces' => array_keys($info['produces']),
      'consumes' => array_keys($info['consumes']),
    ];
  }
  return $manifest;
}
