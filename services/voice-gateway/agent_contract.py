"""Versioned private agent contract; identities originate in authenticated sessions."""
import hashlib
import datetime as dt
import json
import uuid
import re

REQUEST_LIMIT = 24576
RESPONSE_LIMIT = 32768
IDENTITY_KEYS = ('device_id', 'principal_id', 'session_id', 'conversation_id', 'correlation_id')
READ_ONLY_TOOLS = frozenset(('viking_search', 'viking_read', 'viking_browse'))
HEX = re.compile(r'[0-9a-f]{64}')


def sha(value):
    return hashlib.sha256(value.encode('utf-8')).hexdigest()


def encode(value):
    return json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(',', ':')).encode()


def validate_request(value):
    fields = {'v', *IDENTITY_KEYS, 'user_message', 'history', 'approved_profile'}
    if not isinstance(value, dict) or set(value) != fields or value['v'] != 2:
        raise ValueError('agent_request_invalid')
    for key in IDENTITY_KEYS:
        text = value[key]
        if not isinstance(text, str) or not text.strip() or len(text) > 128:
            raise ValueError('agent_identity_invalid')
        if key.endswith('_id') and key not in ('device_id', 'principal_id'):
            if str(uuid.UUID(text)) != text:
                raise ValueError('agent_identity_invalid')
    if value['principal_id'] == 'unmapped':
        raise ValueError('agent_identity_unmapped')
    message = value['user_message']
    if not isinstance(message, str) or not message.strip() or len(message.encode()) > 4096:
        raise ValueError('agent_text_invalid')
    history = value['history']
    if not isinstance(history, list) or len(history) > 16 or len(encode(history)) > 12000:
        raise ValueError('agent_history_invalid')
    for index, item in enumerate(history):
        if (not isinstance(item, dict) or set(item) != {'role', 'content'}
                or item['role'] != ('user' if index % 2 == 0 else 'assistant')
                or not isinstance(item['content'], str) or len(item['content'].encode()) > 4096):
            raise ValueError('agent_history_invalid')
    if len(history) % 2:
        raise ValueError('agent_history_invalid')
    profile = value['approved_profile']
    if (not isinstance(profile, dict) or set(profile) != {'preferred_name'}
            or not isinstance(profile['preferred_name'], str) or not 0 < len(profile['preferred_name']) <= 80):
        raise ValueError('agent_profile_invalid')
    if len(encode(value)) > REQUEST_LIMIT:
        raise ValueError('agent_request_too_large')
    return value


def validate_response(value, request):
    if (not isinstance(value, dict) or value.get('v') != 2 or value.get('status') != 'PASS'
            or value.get('agent_mode') not in ('guarded_agent', 'profile_agent')
            or value.get('request_sha256') != hashlib.sha256(encode(request)).hexdigest()
            or value.get('identity') != {key:request[key] for key in IDENTITY_KEYS}):
        raise ValueError('agent_response_binding_invalid')
    if (not isinstance(value.get('agent_revision'),str) or not re.fullmatch(r'[0-9a-f]{40}',value['agent_revision'])
            or not isinstance(value.get('config_sha256'),str) or not HEX.fullmatch(value['config_sha256'])):
        raise ValueError('agent_source_binding_invalid')
    answer = value.get('answer')
    if not isinstance(answer, str) or not answer.strip() or len(answer.encode()) > 4096:
        raise ValueError('agent_answer_invalid')
    traces = value.get('tool_trace')
    if not isinstance(traces, list) or len(traces) > 8:
        raise ValueError('agent_trace_invalid')
    for trace in traces:
        if (not isinstance(trace, dict) or trace.get('event') != 'agent_tool_result'
                or not valid_tool_trace_scope(value['agent_mode'], trace)
                or trace.get('authenticated_turn_correlation') != request['correlation_id']
                or trace.get('session_id') != request['session_id']
                or trace.get('device_id') != request['device_id']
                or trace.get('principal_binding_sha256') != sha(json.dumps([request['device_id'], request['principal_id']]))
                or trace.get('tool_request_sha256') != sha(request['user_message'])
                or trace.get('agent_revision') != value['agent_revision']
                or trace.get('config_sha256') != value['config_sha256']
                or not isinstance(trace.get('result_sha256'),str) or not HEX.fullmatch(trace['result_sha256'])
                or trace.get('status') not in ('PASS', 'FAIL')):
            raise ValueError('agent_trace_binding_invalid')
        stamp = dt.datetime.fromisoformat(trace['collected_utc'].replace('Z','+00:00'))
        if stamp.tzinfo is None or not 0 <= (dt.datetime.now(dt.timezone.utc)-stamp).total_seconds() <= 60:
            raise ValueError('agent_trace_stale')
    return answer


def valid_tool_trace_scope(mode, trace):
    if mode == 'guarded_agent':
        return trace.get('tool_name') in READ_ONLY_TOOLS and trace.get('operation_class') == 'read_only'
    # Profile tools are authorized by the protected backend policy and native
    # Hermes config, not by a client frame. Do not label write-capable tools read-only.
    return (mode == 'profile_agent' and trace.get('operation_class') == 'configured_profile'
            and isinstance(trace.get('tool_name'), str)
            and re.fullmatch(r'[A-Za-z_][A-Za-z0-9_.-]{0,127}', trace['tool_name']) is not None)
