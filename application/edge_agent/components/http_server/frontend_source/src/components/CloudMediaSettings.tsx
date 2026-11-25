import { For, Show, createMemo, createSignal, onCleanup, onMount } from 'solid-js';
import { fetchConfigFields, fetchMediaModels, saveDeviceSetting, type MediaKind, type MediaModel } from '../api/client';
import { currentLocale, t } from '../i18n';
import { saveConfig } from '../state/config';
import { Banner } from './ui/Banner';
import { Button } from './ui/Button';
import { StaticConfigBlock } from './ui/ConfigBlocks';
import { SelectInput, TextInput } from './ui/FormField';
import { Switch } from './ui/Switch';

const blank = (): MediaModel => ({ name: '', model: '', url: '', kind: 'image', enabled: false });
const isObject = (value: unknown): value is Record<string, unknown> =>
  value !== null && typeof value === 'object' && !Array.isArray(value);
const parseParameters = (text: string) => {
  try {
    const value: unknown = text.trim() ? JSON.parse(text) : {};
    return isObject(value) ? value : undefined;
  } catch {
    return undefined;
  }
};
const options = [
  ['model_field', '模型字段', 'Model field', 'model'],
  ['input_field', '输入字段', 'Input field', 'prompt / input / file'],
  ['result_path', '结果字段路径', 'Result path', 'data.0.url / url / text'],
  ['auth_header', '鉴权请求头', 'Auth header', 'Authorization'],
  ['auth_prefix', '密钥前缀', 'Key prefix', 'Bearer '],
  ['job_path', '任务 ID 路径', 'Job ID path', 'job_id'],
  ['poll_url', '查询任务的 URL', 'Job polling URL', 'https://example.com/jobs/{id}'],
  ['status_path', '任务状态路径', 'Job status path', 'status'],
  ['success_value', '任务成功状态值', 'Success status', 'completed'],
  ['failure_value', '任务失败状态值', 'Failure status', 'failed'],
] as const;

export const CloudMediaSettings = () => {
  const local = (zh: string, en: string) => currentLocale() === 'zh-cn' ? zh : en;
  const label = (kind: MediaKind) => ({ image: local('生图', 'Image generation'), tts: local('语音合成', 'Speech synthesis'), stt: local('语音转写', 'Transcription') })[kind];
  const [models, setModels] = createSignal<MediaModel[]>();
  const [editing, setEditing] = createSignal<MediaModel>();
  const [original, setOriginal] = createSignal('');
  const [key, setKey] = createSignal('');
  const [parameters, setParameters] = createSignal('');
  const [voiceId, setVoiceId] = createSignal<string>();
  const [error, setError] = createSignal('');
  const [busy, setBusy] = createSignal(false);
  const [replyMode, setReplyMode] = createSignal<number>();
  const [savingMode, setSavingMode] = createSignal(false);
  const [modeError, setModeError] = createSignal('');
  const replyModes = () => [local('关闭', 'Off'), local('文字和语音', 'Text and voice'), local('仅语音', 'Voice only')];
  const parsed = createMemo(() => parseParameters(parameters()));
  const mimo = () => {
    const model = editing();
    return model && /^mimo-.*-tts/.test(model.model.trim().toLowerCase()) &&
      /(?:\/v1\/?$|\/chat\/completions(?:\?|$))/.test(model.url.trim());
  };
  const voiceParams = () => {
    const params = parsed();
    return mimo() ? (isObject(params?.audio) ? params.audio : undefined) : params;
  };
  const voiceKey = () => !mimo() && typeof voiceParams()?.voice !== 'string' &&
    typeof voiceParams()?.voice_id === 'string' ? 'voice_id' : 'voice';
  const savedVoice = () => {
    const voice = voiceParams()?.[voiceKey()];
    return typeof voice === 'string' ? voice : '';
  };
  let editor: HTMLFormElement | undefined;
  const refresh = async (quiet = false) => {
    try {
      const next = await fetchMediaModels();
      if (JSON.stringify(next) !== JSON.stringify(models())) setModels(next);
      if (!quiet) setError('');
    }
    catch (e) { if (!quiet) setError((e as Error).message); }
  };
  onMount(() => {
    void refresh();
    void fetchConfigFields(['tts_reply_mode']).then(config => {
      const mode = config.tts_reply_mode;
      if (mode === '0' || mode === '1' || mode === '2') setReplyMode(Number(mode));
      else setModeError(local('设备未提供语音回复设置。', 'Voice reply settings are unavailable on this device.'));
    }).catch(e => setModeError((e as Error).message));
    const focus = () => { if (!busy() && document.visibilityState === 'visible') void refresh(true); };
    const timer = window.setInterval(focus, 8000);
    window.addEventListener('focus', focus);
    onCleanup(() => { window.clearInterval(timer); window.removeEventListener('focus', focus); });
  });
  const edit = (model?: MediaModel) => {
    setEditing(model ? { ...model } : blank());
    setOriginal(model?.name || '');
    setKey('');
    setParameters(model?.parameters ? JSON.stringify(model.parameters) : '');
    setVoiceId(undefined);
    setError('');
    requestAnimationFrame(() => editor?.scrollIntoView({ block: 'nearest' }));
  };
  const patch = (value: Partial<MediaModel>) => setEditing(prev => prev ? { ...prev, ...value } : prev);
  const mutate = async (data: object) => {
    setError(''); setBusy(true);
    try {
      await saveDeviceSetting('/api/media-models', data);
      setEditing(undefined); setKey('');
      await refresh();
    } catch (e) { setError((e as Error).message); }
    finally { setBusy(false); }
  };
  const save = (event: SubmitEvent) => {
    event.preventDefault();
    if (busy() || !editing()) return;
    const profile = { ...editing()!, name: editing()!.name.trim(), url: editing()!.url.trim(), model: editing()!.model.trim() };
    delete profile.api_key_set;
    const params = parsed();
    if (!params) {
      setError(local('附加参数须为 JSON 对象。', 'Extra parameters must be a JSON object.'));
      return;
    }
    profile.parameters = parameters().trim() ? params : undefined;
    if (profile.kind === 'tts' && voiceId() !== undefined && voiceId()!.trim() !== savedVoice()) {
      const voice = voiceId()!.trim();
      if (mimo() && params.audio !== undefined && !isObject(params.audio)) {
        setError(local('请在高级设置中将 audio 改为 JSON 对象。', 'Set audio to a JSON object in the advanced settings.'));
        return;
      }
      const updated = { ...params };
      const fields = mimo() ? { ...voiceParams() } : updated;
      if (voice) fields[voiceKey()] = voice;
      else delete fields[voiceKey()];
      if (mimo()) {
        if (Object.keys(fields).length) updated.audio = fields;
        else delete updated.audio;
      }
      profile.parameters = Object.keys(updated).length ? updated : undefined;
    }
    void mutate({ action: 'save', profile: { ...profile, ...(key() ? { api_key: key() } : {}) } });
  };
  const saveReplyMode = async (input: HTMLInputElement) => {
    const mode = input.valueAsNumber;
    if (savingMode() || mode === replyMode()) return;
    setSavingMode(true); setModeError('');
    try {
      await saveConfig({ tts_reply_mode: String(mode) });
      setReplyMode(mode);
    } catch (e) {
      input.value = String(replyMode() ?? 0);
      setModeError((e as Error).message);
    } finally {
      setSavingMode(false);
    }
  };
  return (
    <StaticConfigBlock title={local('云端图片与语音模型', 'Cloud image and speech models')} class="border-t border-[var(--color-border-subtle)]">
      <div class="space-y-3 pt-2">
        <div class="flex flex-col gap-3 sm:flex-row sm:items-start sm:justify-between">
          <p class="m-0 max-w-xl text-[0.8rem] leading-relaxed text-[var(--color-text-secondary)]">{local(
            '可以在聊天里提供生图或语音合成的接口地址、模型 ID 和文档来创建草稿。密钥在这里填写，启用后即可使用。',
            'Share your image or speech API URL, model ID, and docs in chat to create a draft. Set the key and enable the model here.',
          )}</p>
          <div class="flex flex-wrap gap-2 sm:shrink-0">
            <Button size="sm" class="min-h-11 sm:min-h-0" disabled={busy()} onClick={() => void refresh()}>{t('commandRefresh')}</Button>
            <Button size="sm" class="min-h-11 sm:min-h-0" variant="primary" disabled={busy()} onClick={() => edit()}>{local('添加模型', 'Add model')}</Button>
          </div>
        </div>
        <div class="rounded-lg border border-[var(--color-border-subtle)] p-3 text-sm">
          <label for="tts-reply-mode" class="font-medium">{local('自动语音回复', 'Automatic voice replies')}</label>
          <div class="max-w-md">
            <input id="tts-reply-mode" type="range" min={0} max={2} step={1} value={replyMode() ?? 0}
              aria-valuetext={replyModes()[replyMode() ?? 0]} aria-describedby="tts-reply-hint"
              disabled={replyMode() === undefined || savingMode()} onChange={e => void saveReplyMode(e.currentTarget)}
              class="h-11 w-full cursor-pointer border-0 bg-transparent px-0 py-0 accent-[var(--color-accent)] disabled:opacity-60" />
            <div class="grid grid-cols-3 text-xs text-[var(--color-text-secondary)]" aria-hidden="true">
              <For each={replyModes()}>{(label, index) => (
                <span classList={{ 'text-center': index() === 1, 'text-right': index() === 2,
                  'font-semibold text-[var(--color-text-primary)]': index() === replyMode() }}>{label}</span>
              )}</For>
            </div>
          </div>
          <p id="tts-reply-hint" class="mb-0 mt-2 text-xs leading-relaxed text-[var(--color-text-secondary)]">{local(
            '自动保存。普通回复使用第一个已启用的语音模型。合成失败时会发文字，关闭后仍可用 /tts。',
            'Saves automatically. Uses the first enabled speech model for regular replies. Falls back to text if needed. /tts still works when off.',
          )}</p>
          <Banner kind="error" message={modeError()} class="mt-2 wrap-anywhere" />
        </div>
        <details class="rounded-lg border border-[var(--color-border-subtle)] p-3 text-sm">
          <summary class="cursor-pointer font-medium">{local('从聊天配置语音模型', 'Configure speech models from chat')}</summary>
          <p class="mb-0 mt-2 text-[var(--color-text-secondary)]">{local(
            '在机器人私聊或设备 Web 聊天逐行发送 URL、Key 和 Model ID。设备试合成成功后会保存并启用。',
            'Send URL, Key, and Model ID on separate lines in a private bot chat or Web Chat. The device verifies and enables the model.',
          )}</p>
        </details>
        <Show when={!editing()}>
          <Banner kind="error" message={error()} class="wrap-anywhere" />
        </Show>
        <Show when={models()?.length} fallback={
          <Show when={models()?.length === 0 && !editing()}>
            <p class="m-0 rounded-lg border border-[var(--color-border-subtle)] px-3 py-2.5 text-sm text-[var(--color-text-secondary)]">{local('还没有云端媒体模型。', 'No cloud media models yet.')}</p>
          </Show>
        }>
          <ul class="m-0 list-none divide-y divide-[var(--color-border-subtle)] rounded-lg border border-[var(--color-border-subtle)] p-0">
            <For each={models()}>{model => (
              <li class="flex flex-col gap-2 px-3 py-3 sm:flex-row sm:items-center sm:justify-between">
                <div class="min-w-0">
                  <div class="wrap-anywhere text-sm font-medium"><span translate="no">{model.name}</span> · {label(model.kind)}</div>
                  <div class="mt-1 wrap-anywhere text-xs text-[var(--color-text-secondary)]" translate="no">{model.model}</div>
                  <div class="mt-1 text-xs text-[var(--color-text-secondary)]">
                    {model.enabled ? local('已启用', 'Enabled') : local('草稿 / 已停用', 'Draft / disabled')} · {model.api_key_set ? local('已设置密钥', 'Key saved') : local('未设置密钥', 'No key')}
                  </div>
                </div>
                <div class="flex flex-wrap gap-2 sm:shrink-0">
                  <Button size="sm" class="min-h-11 sm:min-h-0" disabled={busy()} onClick={() => edit(model)}>{local('编辑', 'Edit')}</Button>
                  <Button size="sm" class="min-h-11 sm:min-h-0" variant="danger-ghost" disabled={busy()} onClick={() => {
                    if (window.confirm(local('删除此媒体模型？', 'Delete this media model?'))) void mutate({ action: 'remove', name: model.name });
                  }}>{local('删除', 'Delete')}</Button>
                </div>
              </li>
            )}</For>
          </ul>
        </Show>
        <Show when={editing()}>
          <form ref={editor} class="space-y-3 rounded-lg border border-[var(--color-border-subtle)] p-3 sm:p-4" onSubmit={save}>
            <div class="grid grid-cols-1 gap-3 sm:grid-cols-2">
              <TextInput name="name" spellcheck={false} label={local('配置名称', 'Profile name')} required pattern="(?:[A-Za-z0-9_]|-)+" maxLength={48}
                readOnly={!!original()} value={editing()?.name} onInput={e => patch({ name: e.currentTarget.value })} />
              <SelectInput name="kind" label={local('类型', 'Type')}
                disabled={!!original()} value={editing()?.kind} onChange={e => patch({ kind: e.currentTarget.value as MediaKind })}>
                <For each={['image', 'tts', 'stt'] as const}>{kind => <option value={kind}>{label(kind)}</option>}</For>
              </SelectInput>
              <TextInput name="url" spellcheck={false} label="API URL" type="url" required maxLength={512} value={editing()?.url} onInput={e => patch({ url: e.currentTarget.value })} />
              <TextInput name="model" spellcheck={false} label={local('模型 ID', 'Model ID')} required maxLength={128} value={editing()?.model} onInput={e => patch({ model: e.currentTarget.value })} />
              <Show when={editing()?.kind === 'tts'}>
                <TextInput name="voice_id" spellcheck={false} label={local('音色 ID（选填）', 'Voice ID (optional)')}
                  hint={local('填服务商提供的 ID，留空用默认音色。服务商要求其他字段时可填写附加参数。', 'Enter a voice ID from your provider, or leave blank for its default. Use extra parameters for other fields.')}
                  maxLength={128} value={voiceId() ?? savedVoice()} onInput={e => setVoiceId(e.currentTarget.value)} />
              </Show>
              <TextInput name="api_key" label={local('媒体 API 密钥', 'Media API key')} type="password" autocomplete="new-password"
                placeholder={editing()?.api_key_set ? local('留空保留原密钥', 'Leave blank to keep the key') : ''}
                value={key()} onInput={e => setKey(e.currentTarget.value)} />
              <Switch class="min-h-10 w-fit self-end" label={local('启用该模型', 'Enable this model')}
                checked={!!editing()?.enabled} onChange={enabled => patch({ enabled })} />
            </div>
            <Show when={editing()?.kind === 'tts' && !original()}>
              <Button size="sm" class="min-h-11 sm:min-h-0" onClick={() => patch({
                name: editing()?.name || 'mimo-tts', url: 'https://api.xiaomimimo.com/v1/chat/completions', model: 'mimo-v2.5-tts',
              })}>{local('填入小米 MiMo TTS 配置', 'Use Xiaomi MiMo TTS preset')}</Button>
            </Show>
            <details class="border-t border-[var(--color-border-subtle)] text-sm">
              <summary class="cursor-pointer py-2.5">{local('接口字段与异步任务', 'API fields and async jobs')}</summary>
              <div class="grid grid-cols-1 gap-3 pt-1 sm:grid-cols-2">
                <For each={options}>{([field, zh, en, placeholder], index) => <Show when={index() < 5 || editing()?.kind !== 'stt'}>
                  <TextInput name={field} spellcheck={false} label={local(zh, en)} placeholder={placeholder} value={editing()?.[field]} onInput={e => patch({ [field]: e.currentTarget.value })} />
                </Show>}</For>
                <TextInput name="parameters" spellcheck={false} label={local('附加参数 JSON', 'Extra JSON parameters')} maxLength={1024} value={parameters()} placeholder='{"size":"1024x1024"}' onInput={e => { setParameters(e.currentTarget.value); setError(''); }} />
              </div>
            </details>
            <Banner kind="error" message={error()} class="wrap-anywhere" />
            <div class="flex flex-wrap gap-2">
              <Button type="submit" variant="primary" size="sm" class="min-h-11 sm:min-h-0" disabled={busy()}>{local('保存媒体模型', 'Save media model')}</Button>
              <Button size="sm" class="min-h-11 sm:min-h-0" disabled={busy()} onClick={() => { setEditing(undefined); setKey(''); }}>{t('chatCancel')}</Button>
            </div>
          </form>
        </Show>
      </div>
    </StaticConfigBlock>
  );
};
