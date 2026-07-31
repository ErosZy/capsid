import { definePlugin } from 'h3/generic';

export const constructorPlugin = state => app => {
    state.pluginOrder.push('constructor');
    app.get('/plugins/constructor', () => ({
        order: [ ...state.pluginOrder ],
        source: 'constructor',
    }));
};

export const configurablePlugin = definePlugin((app, options) => {
    app.config;
    options.state.pluginOrder.push(options.name);
    app.get(`/plugins/${options.name}`, () => ({
        order: [ ...options.state.pluginOrder ],
        source: options.name,
    }));
});
