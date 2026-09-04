import { render } from '@testing-library/svelte';
import Image from '$lib/components/Image.svelte';

describe('Image component', () => {
  const src = '/api/assets/abc/thumbnail?size=preview';
  const hdrSrc = '/api/assets/abc/thumbnail?size=preview&hdr=true';

  it('renders a plain image when no HDR derivative is offered', () => {
    const { container } = render(Image, { props: { src, alt: 'test' } });

    expect(container.querySelector('img')).toHaveAttribute('src', src);
    expect(container.querySelector('source')).toBeNull();
  });

  it('offers the HDR AVIF as a <picture> source', () => {
    const { container } = render(Image, { props: { src, hdrSrc, alt: 'test' } });

    const source = container.querySelector('source');
    expect(source).not.toBeNull();
    expect(source).toHaveAttribute('srcset', hdrSrc);
    expect(source).toHaveAttribute('type', 'image/avif');
  });

  it('gates the HDR source on an HDR-capable display', () => {
    const { container } = render(Image, { props: { src, hdrSrc, alt: 'test' } });

    // Without this, SDR displays would receive PQ content they cannot show correctly.
    expect(container.querySelector('source')).toHaveAttribute('media', '(dynamic-range: high)');
  });

  it('keeps the SDR image as the fallback so non-HDR clients still render', () => {
    const { container } = render(Image, { props: { src, hdrSrc, alt: 'test' } });

    const img = container.querySelector('img');
    expect(img).not.toBeNull();
    expect(img).toHaveAttribute('src', src);
  });

  it('orders the HDR source before the fallback image, as <picture> requires', () => {
    const { container } = render(Image, { props: { src, hdrSrc, alt: 'test' } });

    const picture = container.querySelector('picture');
    expect(picture?.children[0].tagName).toBe('SOURCE');
    expect(picture?.children[1].tagName).toBe('IMG');
  });
});
