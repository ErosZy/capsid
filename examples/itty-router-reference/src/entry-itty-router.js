import { createApplication } from './shared-handlers.js';

const application = createApplication('itty-router');

export { application };
export const fetch = application.fetch;
