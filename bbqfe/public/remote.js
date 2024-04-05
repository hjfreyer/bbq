(() => {
    const scripts = document.getElementsByTagName('script');
    const remoteScriptSrc = scripts[scripts.length - 1].getAttribute("src");

    const css = document.createElement("link");
    css.rel = "stylesheet";
    css.href = "https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/css/bootstrap.min.css";
    css.integrity = "sha384-QWTKZyjpPEjISv5WaRU9OFeRpok6YctnYmDr5pNlyT2bRjXh0JMhjY6hW+ALEwIH";
    css.crossOrigin = "anonymous";
    document.head.appendChild(css);

    const script = document.createElement("script");
    script.type = "module";
    script.src = new URL('/remotemain.js', remoteScriptSrc);

    document.head.appendChild(script)
})();

