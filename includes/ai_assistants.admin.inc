<?php

/**
 * @file
 * Assistant admin form helpers.
 */

/**
 * Renders the assistant list admin form.
 */
function ai_assistants_admin_list_form($form, &$form_state) {
  $assistants = ai_assistants_get_assistants();
  $form['assistants'] = [
    '#type' => 'fieldset',
    '#title' => t('Configured assistants'),
  ];

  if (empty($assistants)) {
    $form['assistants']['empty'] = [
      '#markup' => '<p>' . t('No assistants configured yet.') . '</p>',
    ];
  }
  else {
    $rows = [];
    foreach ($assistants as $id => $assistant) {
      $agent_count = count(ai_assistants_get_agent_ids($assistant));
      $rows[] = [
        check_plain($id),
        check_plain(isset($assistant['label']) ? $assistant['label'] : $id),
        check_plain($agent_count > 1 ? $agent_count . ' agents' : (!empty($assistant['agent_id']) ? $assistant['agent_id'] : 'direct')),
        check_plain(isset($assistant['thread_storage']) ? $assistant['thread_storage'] : 'session'),
        l(t('Edit'), 'admin/config/ai/ai-assistants/' . $id . '/edit') . ' | ' . l(t('Delete'), 'admin/config/ai/ai-assistants/' . $id . '/delete'),
      ];
    }
    $form['assistants']['table'] = [
      '#theme' => 'table',
      '#header' => [t('ID'), t('Label'), t('Agent'), t('Storage'), t('Operations')],
      '#rows' => $rows,
    ];
  }

  $form['actions'] = ['#type' => 'actions'];
  $form['actions']['add'] = [
    '#type' => 'link',
    '#title' => t('Add assistant'),
    '#href' => 'admin/config/ai/ai-assistants/add',
    '#attributes' => ['class' => ['button', 'button-primary']],
  ];

  return $form;
}

/**
 * Add/edit form.
 */
function ai_assistants_admin_edit_form($form, &$form_state, $assistant_id = '') {
  $assistants = ai_assistants_get_assistants();
  $is_edit = !empty($assistant_id) && isset($assistants[$assistant_id]);
  $assistant = $is_edit ? $assistants[$assistant_id] : [];

  $form['label'] = [
    '#type' => 'textfield',
    '#title' => t('Label'),
    '#default_value' => isset($assistant['label']) ? $assistant['label'] : '',
    '#required' => TRUE,
  ];

  $form['assistant_id'] = [
    '#type' => 'machine_name',
    '#title' => t('Assistant ID'),
    '#default_value' => $is_edit ? $assistant_id : '',
    '#machine_name' => [
      'exists' => 'ai_assistants_admin_exists',
      'source' => ['label'],
    ],
    '#required' => TRUE,
    '#disabled' => $is_edit,
  ];

  $form['system_prompt'] = [
    '#type' => 'textarea',
    '#title' => t('System prompt'),
    '#default_value' => isset($assistant['system_prompt']) ? $assistant['system_prompt'] : '',
    '#rows' => 8,
    '#description' => t('Primary assistant instructions.'),
  ];

  $pre_action_description = t('Used as the router system prompt in router/multi_agent modes (decides which backing agent(s) handle each request) and as an extra system message in direct mode. Leave empty to use the default router template. The template supports the placeholders [examples] and [usage_instructions], which are filled at runtime by aggregating the corresponding fields from each available agent. The model also receives the full agent manifest (description, tools, declared orchestration metadata) in the user message, so the prompt itself never needs to name specific agents.');
  if (function_exists('ai_assistants_default_router_prompt')) {
    $pre_action_description .= '<br><br><strong>' . t('Current default:') . '</strong><br><pre style="white-space:pre-wrap;font-size:0.85em;background:#f7f7f7;padding:0.5em;border:1px solid #ddd;max-height:240px;overflow:auto;">' . check_plain(ai_assistants_default_router_prompt()) . '</pre>';
  }
  $form['pre_action_prompt'] = [
    '#type' => 'textarea',
    '#title' => t('Pre-action prompt'),
    '#default_value' => isset($assistant['pre_action_prompt']) ? $assistant['pre_action_prompt'] : '',
    '#rows' => 10,
    '#description' => $pre_action_description,
  ];

  $form['secured_system_prompt'] = [
    '#type' => 'textarea',
    '#title' => t('Secured system prompt'),
    '#default_value' => isset($assistant['secured_system_prompt']) ? $assistant['secured_system_prompt'] : '',
    '#rows' => 4,
    '#description' => t('Extra prompt inserted after user content for harder-to-override instructions.'),
  ];

  $form['route_mode'] = [
    '#type' => 'select',
    '#title' => t('Route mode'),
    '#options' => ai_assistants_route_mode_options(),
    '#default_value' => isset($assistant['route_mode']) ? $assistant['route_mode'] : '',
    '#empty_option' => t('Auto'),
    '#empty_value' => '',
    '#description' => t('Choose direct response, one agent, or multiple agents.'),
  ];


  $form['model'] = [
    '#type' => 'select',
    '#title' => t('Direct answer model'),
    '#options' => function_exists('ai_get_all_chat_models') ? ai_get_all_chat_models() : [],
    '#default_value' => isset($assistant['model']) ? $assistant['model'] : ai_assistants_default_model($assistant),
    '#description' => t('Model used for direct answers and for all backing agents. Overrides each agent\'s individual model setting.'),
  ];

  $form['temperature'] = [
    '#type' => 'number',
    '#title' => t('Temperature'),
    '#default_value' => isset($assistant['temperature']) ? $assistant['temperature'] : 0.3,
    '#min' => 0,
    '#max' => 2,
    '#step' => 0.1,
  ];

  $form['max_tokens'] = [
    '#type' => 'number',
    '#title' => t('Max tokens'),
    '#default_value' => isset($assistant['max_tokens']) ? (int) $assistant['max_tokens'] : 1024,
    '#min' => 1,
    '#step' => 1,
  ];

  $form['agent_id'] = [
    '#type' => 'select',
    '#title' => t('Primary backing agent'),
    '#options' => ai_agents_list_agent_options(),
    '#default_value' => isset($assistant['agent_id']) ? $assistant['agent_id'] : '',
    '#required' => FALSE,
    '#empty_option' => t('— None —'),
    '#empty_value' => '',
    '#description' => t('Primary AI agent config. Leave empty for direct mode.'),
  ];

  $form['agent_ids'] = [
    '#type' => 'checkboxes',
    '#title' => t('Additional backing agents'),
    '#options' => ai_agents_list_agent_options(),
    '#default_value' => isset($assistant['agent_ids']) && is_array($assistant['agent_ids']) ? array_values($assistant['agent_ids']) : [],
    '#description' => t('Select one or more agents for multi-agent routing.'),
  ];

  $rag_index_options = ai_assistants_rag_index_options();
  if (!empty($rag_index_options)) {
    $form['rag_search'] = [
      '#type' => 'fieldset',
      '#title' => t('RAG search'),
      '#description' => t('Select which vector indexes this assistant should search when its backing agents call ai_search_rag_search. This assistant-level setting overrides agent defaults and is the right place to pin a site chatbot to specific content indexes.'),
      '#collapsible' => TRUE,
      '#collapsed' => FALSE,
    ];
    $form['rag_search']['rag_index_ids'] = [
      '#type' => 'checkboxes',
      '#title' => t('Vector indexes'),
      '#options' => $rag_index_options,
      '#default_value' => isset($assistant['rag_index_ids']) && is_array($assistant['rag_index_ids']) ? array_values($assistant['rag_index_ids']) : [],
      '#description' => t('Leave empty to fall back to the selected backing agent\'s default RAG indexes.'),
    ];
  }

  $form['thread_storage'] = [
    '#type' => 'select',
    '#title' => t('Thread storage'),
    '#options' => [
      'session' => t('Session'),
      'database' => t('Database'),
      'none' => t('None'),
    ],
    '#default_value' => isset($assistant['thread_storage']) ? $assistant['thread_storage'] : 'session',
  ];

  $form['thread_ttl'] = [
    '#type' => 'number',
    '#title' => t('Thread TTL'),
    '#default_value' => isset($assistant['thread_ttl']) ? (int) $assistant['thread_ttl'] : 0,
    '#min' => 0,
    '#step' => 1,
    '#description' => t('Seconds before a thread expires. 0 = never expire.'),
  ];

  $form['allowed_roles'] = [
    '#type' => 'checkboxes',
    '#title' => t('Allowed roles'),
    '#options' => user_roles(),
    '#default_value' => isset($assistant['allowed_roles']) && is_array($assistant['allowed_roles']) ? array_values($assistant['allowed_roles']) : [],
  ];

  $form['status_events'] = [
    '#type' => 'checkbox',
    '#title' => t('Animate latest response'),
    '#default_value' => !empty($assistant['status_events']),
    '#description' => t('Reveal the newest assistant answer word by word in the chat UI.'),
  ];

  $form['actions'] = ['#type' => 'actions'];
  $form['actions']['submit'] = [
    '#type' => 'submit',
    '#value' => t('Save'),
  ];

  return $form;
}

/**
 * Delete form.
 */
function ai_assistants_admin_delete_form($form, &$form_state, $assistant_id = '') {
  $assistants = ai_assistants_get_assistants();
  if (empty($assistant_id) || empty($assistants[$assistant_id])) {
    return ['#markup' => '<p>' . t('Assistant not found.') . '</p>'];
  }

  $form['assistant_id'] = ['#type' => 'value', '#value' => $assistant_id];
  return confirm_form(
    $form,
    t('Delete assistant %label?', ['%label' => isset($assistants[$assistant_id]['label']) ? $assistants[$assistant_id]['label'] : $assistant_id]),
    'admin/config/ai/ai-assistants',
    t('This removes the assistant config only.'),
    t('Delete'),
    t('Cancel')
  );
}

/**
 * Save handler.
 */
function ai_assistants_admin_edit_form_submit($form, &$form_state) {
  $assistants = ai_assistants_get_assistants();
  $assistant_id = trim((string) $form_state['values']['assistant_id']);
  $agent_ids = isset($form_state['values']['agent_ids']) && is_array($form_state['values']['agent_ids']) ? array_values(array_filter($form_state['values']['agent_ids'])) : [];
  $rag_index_ids = isset($form_state['values']['rag_index_ids']) && is_array($form_state['values']['rag_index_ids']) ? array_values(array_filter($form_state['values']['rag_index_ids'])) : [];
  $assistants[$assistant_id] = [
    'label' => trim((string) $form_state['values']['label']),
    'system_prompt' => trim((string) $form_state['values']['system_prompt']),
    'pre_action_prompt' => trim((string) $form_state['values']['pre_action_prompt']),
    'secured_system_prompt' => trim((string) $form_state['values']['secured_system_prompt']),
    'route_mode' => trim((string) $form_state['values']['route_mode']),
    'model' => trim((string) $form_state['values']['model']),
    'temperature' => (float) $form_state['values']['temperature'],
    'max_tokens' => (int) $form_state['values']['max_tokens'],
    'agent_id' => trim((string) $form_state['values']['agent_id']),
    'agent_ids' => $agent_ids,
    'rag_index_ids' => $rag_index_ids,
    'thread_storage' => trim((string) $form_state['values']['thread_storage']),
    'thread_ttl' => (int) $form_state['values']['thread_ttl'],
    'allowed_roles' => array_values(array_filter($form_state['values']['allowed_roles'])),
    'status_events' => !empty($form_state['values']['status_events']),
  ];
  ai_assistants_save_assistants($assistants);
  backdrop_set_message(t('Assistant saved.'));
  $form_state['redirect'] = 'admin/config/ai/ai-assistants';
}

/**
 * Delete handler.
 */
function ai_assistants_admin_delete_form_submit($form, &$form_state) {
  $assistant_id = (string) $form_state['values']['assistant_id'];
  if (ai_assistants_delete($assistant_id)) {
    backdrop_set_message(t('Assistant deleted.'));
  }
  $form_state['redirect'] = 'admin/config/ai/ai-assistants';
}
