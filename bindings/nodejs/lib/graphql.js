'use strict';

const { GraphQLLayer: GraphQLLayerNative } = require('../build/Release/graphql.node');

/**
 * GraphQL layer for WaveDB
 *
 * Provides schema definition, query, and mutation operations
 * on top of WaveDB's hierarchical key-value store.
 */
class GraphQLLayer {
  /**
   * Create a new GraphQL layer
   *
   * @param {string|null} path - Database path (null for in-memory)
   * @param {Object} [options] - Configuration options
   * @param {boolean} [options.enablePersist=true] - Enable persistence
   * @param {number} [options.chunkSize] - HBTrie chunk size
   * @param {number} [options.workerThreads] - Number of worker threads
   */
  constructor(path, options = {}) {
    if (path === undefined || path === null) {
      this._layer = new GraphQLLayerNative(null, options);
    } else if (typeof path !== 'string') {
      throw new TypeError('Path must be a string or null');
    } else {
      this._layer = new GraphQLLayerNative(path, options);
    }
    this._closed = false;
  }

  /**
   * Parse a GraphQL schema definition (SDL)
   *
   * @param {string} sdl - Schema definition language string
   * @throws {Error} If parsing fails
   */
  parseSchema(sdl) {
    if (this._closed) throw new Error('GraphQL layer is closed');
    if (typeof sdl !== 'string') throw new TypeError('SDL must be a string');
    this._layer.parseSchema(sdl);
  }

  /**
   * Execute a GraphQL query synchronously
   *
   * @param {string} query - GraphQL query string
   * @returns {Object} Result object with { success, data, errors }
   */
  querySync(query) {
    if (this._closed) throw new Error('GraphQL layer is closed');
    if (typeof query !== 'string') throw new TypeError('Query must be a string');
    return this._layer.querySync(query);
  }

  /**
   * Execute a GraphQL mutation synchronously
   *
   * @param {string} mutation - GraphQL mutation string
   * @returns {Object} Result object with { success, data, errors }
   */
  mutateSync(mutation) {
    if (this._closed) throw new Error('GraphQL layer is closed');
    if (typeof mutation !== 'string') throw new TypeError('Mutation must be a string');
    return this._layer.mutateSync(mutation);
  }

  /**
   * Execute a GraphQL query asynchronously
   *
   * @param {string} query - GraphQL query string
   * @returns {Promise<Object>} Result object with { success, data, errors }
   */
  query(query) {
    if (this._closed) return Promise.reject(new Error('GraphQL layer is closed'));
    if (typeof query !== 'string') return Promise.reject(new TypeError('Query must be a string'));
    return new Promise((resolve, reject) => {
      this._layer.query(query, (err, result) => {
        if (err) reject(err);
        else resolve(result);
      });
    });
  }

  /**
   * Execute a GraphQL mutation asynchronously
   *
   * @param {string} mutation - GraphQL mutation string
   * @returns {Promise<Object>} Result object with { success, data, errors }
   */
  mutate(mutation) {
    if (this._closed) return Promise.reject(new Error('GraphQL layer is closed'));
    if (typeof mutation !== 'string') return Promise.reject(new TypeError('Mutation must be a string'));
    return new Promise((resolve, reject) => {
      this._layer.mutate(mutation, (err, result) => {
        if (err) reject(err);
        else resolve(result);
      });
    });
  }

  /**
   * Close the GraphQL layer and release resources
   */
  close() {
    if (!this._closed) {
      this._layer.close();
      this._closed = true;
    }
  }
}

module.exports = { GraphQLLayer };