<script lang="ts">
  import { isFirefox } from '#lib/utils/asset-utils.ts';
  import { cancelImageUrl } from '#lib/utils/sw-messaging.ts';
  import { onDestroy, untrack } from 'svelte';
  import type { HTMLImgAttributes } from 'svelte/elements';

  type Props = Omit<HTMLImgAttributes, 'onload' | 'onerror'> & {
    src: string | undefined;
    /**
     * Optional HDR (10-bit AVIF) derivative.
     *
     * Offered through a `<picture>` source that is gated on both AVIF support
     * and an HDR-capable display, so SDR displays and older browsers keep
     * receiving the ordinary `src` untouched.
     */
    hdrSrc?: string;
    onStart?: () => void;
    onLoad?: () => void;
    onError?: (error: Error) => void;
    ref?: HTMLImageElement;
  };

  let { src, hdrSrc, onStart, onLoad, onError, ref = $bindable(), ...rest }: Props = $props();

  let capturedSource: string | undefined = $state();
  let capturedHdrSource: string | undefined = $state();
  let loaded = $state(false);
  let destroyed = false;

  $effect(() => {
    if (src === undefined || capturedSource !== undefined) {
      return;
    }

    capturedSource = src;
    // Captured alongside src so both stay consistent for the lifetime of the
    // element, matching the existing {#key} behaviour.
    capturedHdrSource = hdrSrc;
    untrack(() => {
      onStart?.();
    });
  });

  onDestroy(() => {
    destroyed = true;
    if (capturedSource !== undefined) {
      cancelImageUrl(capturedSource);
    }
  });

  const completeLoad = () => {
    if (destroyed) {
      return;
    }
    loaded = true;
    onLoad?.();
  };

  const handleLoad = () => {
    if (destroyed || !src) {
      return;
    }

    if (isFirefox && ref) {
      ref.decode().then(completeLoad).catch(completeLoad);
      return;
    }

    completeLoad();
  };

  const handleError = () => {
    if (destroyed || !src) {
      return;
    }
    onError?.(new Error(`Failed to load image: ${src}`));
  };
</script>

{#if capturedSource}
  {#key capturedSource}
    <picture>
      {#if capturedHdrSource}
        <!--
          Selected only when the browser decodes AVIF *and* the display can
          actually show high dynamic range. Everything else falls through to the
          <img> below, which is unchanged.
        -->
        <source type="image/avif" media="(dynamic-range: high)" srcset={capturedHdrSource} />
      {/if}
      <img
        bind:this={ref}
        src={capturedSource}
        {...rest}
        style:visibility={isFirefox && !loaded ? 'hidden' : undefined}
        onload={handleLoad}
        onerror={handleError}
      />
    </picture>
  {/key}
{/if}
