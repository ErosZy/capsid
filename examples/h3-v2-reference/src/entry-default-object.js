import { createApplication } from './app.js';

const app = createApplication();

export default { fetch: app.fetch };
