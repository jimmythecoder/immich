<script lang="ts">
  import Image from '$lib/components/Image.svelte';
  import type { AdaptiveImageLoader, ImageQuality } from '$lib/utils/adaptive-image-loader.svelte';

  type Props = {
    adaptiveImageLoader: AdaptiveImageLoader;
    quality: ImageQuality;
    src: string | undefined;
    /** Optional HDR derivative, offered via <picture> when the display supports it. */
    hdrSrc?: string;
    alt?: string;
    role?: string;
    ref?: HTMLImageElement;
    width: string;
    height: string;
  };

  let { adaptiveImageLoader, quality, src, hdrSrc, alt = '', role, ref = $bindable(), width, height }: Props = $props();
</script>

{#key adaptiveImageLoader}
  <div class="absolute top-0" style:width style:height>
    <Image
      {src}
      {hdrSrc}
      onStart={() => adaptiveImageLoader.onStart(quality)}
      onLoad={() => adaptiveImageLoader.onLoad(quality)}
      onError={() => adaptiveImageLoader.onError(quality)}
      bind:ref
      class="pointer-events-auto size-full bg-transparent"
      {alt}
      {role}
      draggable={false}
      data-testid={quality}
    />
  </div>
{/key}
