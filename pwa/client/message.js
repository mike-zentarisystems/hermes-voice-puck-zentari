/* A bounded Markdown subset built only from DOM nodes. HTML stays literal text. */
(() => {
  function inline(parent, text) {
    const pattern = /\*\*([^*\n]+)\*\*|`([^`\n]+)`|\[([^\]\n]+)\]\(([^\s)]+)\)/g;
    let end = 0;
    for (const match of text.matchAll(pattern)) {
      parent.append(document.createTextNode(text.slice(end, match.index)));
      let node;
      if (match[1]) { node = document.createElement('b'); node.textContent = match[1]; }
      else if (match[2]) { node = document.createElement('code'); node.textContent = match[2]; }
      else {
        try {
          const url = new URL(match[4]);
          if (!['https:', 'http:'].includes(url.protocol)) throw Error();
          node = document.createElement('a'); node.href = url.href;
          node.target = '_blank'; node.rel = 'noopener noreferrer'; node.textContent = match[3];
        } catch { node = document.createTextNode(match[0]); }
      }
      parent.append(node); end = match.index + match[0].length;
    }
    parent.append(document.createTextNode(text.slice(end)));
  }
  window.renderMessage = text => {
    const content = document.createElement('div'); content.className = 'message-content';
    let code = null, list = null, listKind = '';
    for (const line of text.split('\n')) {
      if (line.startsWith('```')) {
        if (code) code = null;
        else { const pre = document.createElement('pre'); code = document.createElement('code'); pre.append(code); content.append(pre); }
        list = null; continue;
      }
      if (code) { code.append(document.createTextNode(line + '\n')); continue; }
      const bullet = line.match(/^\s*(?:[-*] |\d+[.)] )(.*)$/);
      if (bullet) {
        const kind = /^\s*\d/.test(line) ? 'ol' : 'ul';
        if (!list || kind !== listKind) { list = document.createElement(kind); listKind = kind; content.append(list); }
        const item = document.createElement('li'); inline(item, bullet[1]); list.append(item); continue;
      }
      list = null;
      if (!line.trim()) continue;
      const heading = line.match(/^#{1,6}\s+(.+)$/);
      const node = document.createElement(heading ? 'h3' : 'p');
      inline(node, heading ? heading[1] : line); content.append(node);
    }
    return content;
  };
})();
