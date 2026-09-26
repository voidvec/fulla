<script setup lang="ts">
import AppModal from './AppModal.vue'
import AppButton from './AppButton.vue'

defineProps<{
  open: boolean
  title?: string
  /** Pre-rendered body text; the caller interpolates entity names via its
      own i18n before passing it in (this shared component carries no
      app-specific message keys). */
  message?: string
  confirmLabel?: string
  cancelLabel?: string
  /** Danger styling on the confirm button (destructive actions). */
  danger?: boolean
  /** Disables both buttons while the confirmed action is running. */
  busy?: boolean
}>()

const emit = defineEmits<{
  confirm: []
  cancel: []
}>()
</script>

<template>
  <!-- Shared destructive-action confirm (#181): Esc, backdrop click and the
       header X all surface as `cancel` so callers need exactly two handlers.
       Title defaults to the shared ui.confirm.title key; buttons carry
       stable data-testids for the e2e suites (the native window.confirm
       handlers they replaced needed page.on('dialog')). -->
  <AppModal :open="open" :title="title || $t('ui.confirm.title')" size="sm" @close="emit('cancel')">
    <p class="text-sm text-neutral-600">{{ message }}</p>
    <template #footer>
      <AppButton
        variant="secondary"
        :disabled="busy"
        data-testid="confirm-dialog-cancel"
        @click="emit('cancel')"
      >
        {{ cancelLabel || $t('ui.confirm.cancel') }}
      </AppButton>
      <AppButton
        :variant="danger ? 'danger' : 'primary'"
        :loading="busy"
        data-testid="confirm-dialog-confirm"
        @click="emit('confirm')"
      >
        {{ confirmLabel || $t('ui.confirm.confirm') }}
      </AppButton>
    </template>
  </AppModal>
</template>
