import { createApplication } from './app.js';

const app = createApplication();

export default {
    fetch(request) {
        return app.fetch(request);
    },
};
